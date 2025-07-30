// ESP32-S3 LoRa Gateway Repeater for BitChat - Improved Message Handling
// Implements message queuing, non-blocking operations, and better collision avoidance

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <RadioLib.h>
#include <map>
#include <set>
#include <queue>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "../include/hw_pins.h"
#include "../include/bridge.h"

// LoRa Radio setup for Heltec WiFi LoRa 32 V3 (SX1262)
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// BLE Service and Characteristic UUIDs
#define GATEWAY_SERVICE_UUID     "7A1B0000-6B2E-46E8-8D2D-6AA2E5A4F001"
#define GATEWAY_TX_CHAR_UUID     "7A1B0001-6B2E-46E8-8D2D-6AA2E5A4F001"  // ESP32 -> iOS
#define GATEWAY_RX_CHAR_UUID     "7A1B0002-6B2E-46E8-8D2D-6AA2E5A4F001"  // iOS -> ESP32
#define GATEWAY_CONFIG_CHAR_UUID "7A1B0003-6B2E-46E8-8D2D-6AA2E5A4F001"  // Config/Stats

// LoRa parameters for US915
#define LORA_FREQUENCY    915.0  // MHz
#define LORA_BANDWIDTH    125.0  // kHz
#define LORA_SPREADING    7      // SF7
#define LORA_CODING_RATE  5      // 4/5
#define LORA_TX_POWER     20     // dBm
#define LORA_PREAMBLE     8

// Cache configuration
const uint32_t CACHE_TIMEOUT_MS = 30000;     // 30 seconds
const size_t MAX_CACHE_SIZE = 100;           // Maximum cache entries

// Message deduplication cache - now tracks source gateway too
struct MessageInfo {
    uint64_t msg_id;
    uint32_t gw_id;  // Original sender
    uint32_t timestamp;
};
std::map<uint64_t, MessageInfo> messageCache;  // msg_id -> MessageInfo

// Track messages we originated
std::set<uint64_t> ourMessages;  // Messages we sent

// Gateway configuration
uint32_t gatewayId;
uint8_t defaultTTL = 6;
String gatewayName;
uint8_t deviceNumber = 0;

// BLE objects
NimBLEServer* pServer = nullptr;
NimBLEService* pService = nullptr;
NimBLECharacteristic* pTxCharacteristic = nullptr;
NimBLECharacteristic* pRxCharacteristic = nullptr;
NimBLECharacteristic* pConfigCharacteristic = nullptr;

// State variables
bool deviceConnected = false;
uint32_t bleRxCount = 0;
uint32_t bleTxCount = 0;
uint32_t loraRxCount = 0;
uint32_t loraTxCount = 0;
volatile bool loraRxFlag = false;

// Status broadcasting
unsigned long lastStatusBroadcast = 0;
const unsigned long STATUS_BROADCAST_INTERVAL = 10000; // 10 seconds
std::vector<String> connectedDeviceNicknames;

// Forward declarations (only those that don't depend on QueuedMessage)
void sendLoRaMessage(const uint8_t* data, size_t len);
void cleanupMessageCache();
bool isDuplicate(uint64_t msgId);
void updateStats();
void onLoRaReceive();
void processLoRaReceive();
void processSerialCommand();
void broadcastGatewayStatus();
String getMessageTypeDescription(const uint8_t* payload, size_t len);
bool isDuplicateLocked(uint64_t msgId);

// Message queue structures
struct QueuedMessage {
    uint8_t data[256];
    size_t len;
    uint32_t receiveTime;
    float rssi;
    float snr;
    bool isForRebroadcast;
    uint32_t scheduledTransmitTime;  // For collision avoidance
};

// Forward declarations that depend on QueuedMessage (must come after struct definition)
void processLoRaMessage(QueuedMessage* msg);
void processBleMessage(QueuedMessage* msg);

// Thread-safe message queues
QueueHandle_t loraRxQueue;
QueueHandle_t loraTxQueue;
QueueHandle_t bleRxQueue;
QueueHandle_t bleTxQueue;

// Semaphores for thread safety
SemaphoreHandle_t radioMutex;
SemaphoreHandle_t bleMutex;
SemaphoreHandle_t cacheMutex;

// Task handles
TaskHandle_t loraRxTaskHandle;
TaskHandle_t loraTxTaskHandle;
TaskHandle_t bleTaskHandle;

// Performance metrics
volatile uint32_t droppedMessages = 0;
volatile uint32_t queueOverflows = 0;
volatile uint32_t collisionBackoffs = 0;

// Improved collision avoidance
const uint32_t BASE_BACKOFF_MS = 10;
const uint32_t MAX_BACKOFF_MS = 500;
uint32_t currentBackoff = BASE_BACKOFF_MS;

// BLE Server Callbacks
class MyServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) {
        deviceConnected = true;
        Serial.printf("[%s] *** BLE Client Connected ***\n", gatewayName.c_str());
        Serial.printf("Connected clients: %d\n", pServer->getConnectedCount());
        updateStats();
    }

    void onDisconnect(NimBLEServer* pServer) {
        deviceConnected = false;
        Serial.printf("[%s] *** BLE Client Disconnected ***\n", gatewayName.c_str());
        Serial.printf("Connected clients: %d\n", pServer->getConnectedCount());
        
        // Clear connected device nicknames when all clients disconnect
        if (pServer->getConnectedCount() == 0) {
            connectedDeviceNicknames.clear();
            Serial.printf("[%s] Cleared connected device list\n", gatewayName.c_str());
        }
        
        pServer->startAdvertising();
        Serial.println("BLE advertising restarted");
    }
};

// RX Characteristic Callback - receives from iOS app and forwards to LoRa
class MyRxCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) {
        std::string rxValue = pCharacteristic->getValue();
        
        if (rxValue.length() > 0) {
            bleRxCount++;
            Serial.printf("\n[%s] ━━━ BLE RX #%lu: %d bytes ━━━\n", 
                         gatewayName.c_str(), bleRxCount, rxValue.length());
            
            // Get message type description
            String msgType = getMessageTypeDescription((const uint8_t*)rxValue.data(), rxValue.length());
            Serial.printf("Message Type: %s\n", msgType.c_str());
            
            // Try to extract nickname from BitChat message
            // BitChat messages have a specific binary format we need to parse
            // For now, we'll look for system messages that announce connections
            String messageStr((const char*)rxValue.data(), rxValue.length());
            
            // Look for patterns like "nickname connected" or messages from specific senders
            // This is a simplified approach - in production you'd parse the actual BitChat protocol
            if (messageStr.indexOf(" connected") > 0 && messageStr.indexOf("system") < 0) {
                int spacePos = messageStr.indexOf(" connected");
                if (spacePos > 0) {
                    String nickname = messageStr.substring(0, spacePos);
                    // Clean up the nickname
                    nickname.trim();
                    if (nickname.length() > 0 && nickname != "system") {
                        // Check if we already have this nickname
                        bool found = false;
                        for (const auto& n : connectedDeviceNicknames) {
                            if (n == nickname) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            connectedDeviceNicknames.push_back(nickname);
                            Serial.printf("[%s] Added connected device: %s\n", 
                                        gatewayName.c_str(), nickname.c_str());
                        }
                    }
                }
            }
            
            // Forward to LoRa
            sendLoRaMessage((const uint8_t*)rxValue.data(), rxValue.length());
            
            // Send echo response
            if (pTxCharacteristic) {
                String response = "Echo: " + String(bleRxCount);
                pTxCharacteristic->setValue(response.c_str());
                pTxCharacteristic->notify();
                Serial.printf("Sent echo response: '%s'\n", response.c_str());
            }
        }
    }
};

// Improved RX callback for BLE
class ImprovedRxCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) {
        std::string rxValue = pCharacteristic->getValue();
        
        if (rxValue.length() > 0) {
            QueuedMessage msg;
            memcpy(msg.data, rxValue.data(), rxValue.length());
            msg.len = rxValue.length();
            msg.receiveTime = millis();
            
            if (xQueueSend(bleRxQueue, &msg, 0) != pdTRUE) {
                droppedMessages++;
                Serial.println("[WARNING] BLE RX queue full!");
            }
        }
    }
};

// Helper function to describe message type
String getMessageTypeDescription(const uint8_t* payload, size_t len) {
    if (len == 0) return "Empty";
    
    // Check for STATUS messages
    if (len >= 6) {
        String payloadStr = String((char*)payload).substring(0, 6);
        if (payloadStr == "STATUS") {
            return "Gateway Status Broadcast";
        }
    }
    
    // Check for text patterns
    String preview((char*)payload, min(len, (size_t)50));
    if (preview.indexOf("Echo:") == 0) {
        return "Echo Response";
    } else if (preview.indexOf("Heartbeat:") == 0) {
        return "Heartbeat";
    } else if (preview.indexOf("Test message") >= 0) {
        return "Test Message";
    } else if (preview.indexOf(" connected") > 0) {
        return "Connection Announcement";
    } else if (preview.indexOf(" disconnected") > 0) {
        return "Disconnection Announcement";
    }
    
    // Default to showing first few readable characters
    return "BitChat Message";
}

// Send message over LoRa
void sendLoRaMessage(const uint8_t* data, size_t len) {
    // Check packet size limit
    if (len > 200) {
        Serial.printf("[%s] ERROR: Packet too long (%d bytes), max 200\n", 
                     gatewayName.c_str(), len);
        return;
    }
    
    // DEBUG: Log incoming data
    Serial.printf("\n│ DEBUG sendLoRaMessage: Received %d bytes to send    │\n", len);
    Serial.printf("│ DEBUG sendLoRaMessage: First 30 chars: '");
    for (size_t i = 0; i < min(len, (size_t)30); i++) {
        Serial.printf("%c", data[i]);
    }
    Serial.printf("'│\n");
    
    // Wrap in bridge header
    Frame frame;
    memset(&frame, 0, sizeof(Frame));
    
    frame.hdr.magic = BRIDGE_MAGIC;
    frame.hdr.ver = 1;
    frame.hdr.ttl = defaultTTL;
    frame.hdr.msg_id = ((uint64_t)random(0xFFFFFFFF) << 32) | millis();
    frame.hdr.gw_id = gatewayId;  // Our gateway ID
    frame.hdr.frag_idx = 0;
    frame.hdr.frag_total = 1;
    frame.hdr.payload_len = len;
    frame.hdr.hop = 0;
    frame.hdr.crc16 = 0;
    
    frame.len = len;
    memcpy(frame.data, data, len);
    
    // DEBUG: Verify payload in frame
    Serial.printf("│ DEBUG: frame.hdr.payload_len set to: %d            │\n", frame.hdr.payload_len);
    
    // Track that we sent this message
    ourMessages.insert(frame.hdr.msg_id);
    
    // Add to cache
    MessageInfo info = {frame.hdr.msg_id, gatewayId, millis()};
    messageCache[frame.hdr.msg_id] = info;
    
    // Transmit over LoRa
    size_t totalLen = sizeof(BridgeHdr) + sizeof(uint16_t) + frame.len;
    
    Serial.println("┌─── BLE → LoRa TRANSMISSION ──────────────────────────┐");
    Serial.printf("│ Source: BLE Client via %s                           │\n", gatewayName.c_str());
    Serial.printf("│ Destination: LoRa Mesh Network                       │\n");
    Serial.printf("│ Message ID: 0x%016llX                       │\n", frame.hdr.msg_id);
    Serial.printf("│ Gateway ID: 0x%08X                                 │\n", gatewayId);
    Serial.printf("│ TTL: %d, Hop: %d                                    │\n", frame.hdr.ttl, frame.hdr.hop);
    Serial.printf("│ Payload Size: %d bytes                              │\n", len);
    Serial.printf("│ Message Type: %s                                     │\n", getMessageTypeDescription(data, len).c_str());
    
    // Show payload preview
    Serial.print("│ Data Preview: ");
    for (int i = 0; i < min(len, (size_t)16); i++) {
        if (data[i] >= 32 && data[i] <= 126) {
            Serial.printf("%c", data[i]);
        } else {
            Serial.printf(".");
        }
    }
    if (len > 16) Serial.print("...");
    for (int i = 0; i < (50 - min(len, (size_t)16)); i++) Serial.print(" ");
    Serial.println("│");
    
    Serial.println("└──────────────────────────────────────────────────────┘");
    
    Serial.printf("[%s] Transmitting %d bytes over LoRa...\n", gatewayName.c_str(), totalLen);
    
    int state = radio.transmit((uint8_t*)&frame, totalLen);
    
    if (state == RADIOLIB_ERR_NONE) {
        loraTxCount++;
        Serial.printf("[%s] ✓ LoRa TX success!\n", gatewayName.c_str());
    } else {
        Serial.printf("[%s] ✗ LoRa TX failed: %d\n", gatewayName.c_str(), state);
    }
    
    // Return to receive mode
    radio.startReceive();
    updateStats();
}

// Improved LoRa interrupt handler - just adds to queue
void IRAM_ATTR onLoRaReceive() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    // Notify the LoRa RX task
    vTaskNotifyGiveFromISR(loraRxTaskHandle, &xHigherPriorityTaskWoken);
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// LoRa RX Task - runs on Core 0
void loraRxTask(void* parameter) {
    uint8_t buffer[256];
    
    while (true) {
        // Wait for interrupt notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        if (xSemaphoreTake(radioMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            size_t len = radio.getPacketLength();
            
            if (len > 0 && len <= sizeof(buffer)) {
                int state = radio.readData(buffer, len);
                
                if (state == RADIOLIB_ERR_NONE) {
                    QueuedMessage msg;
                    memcpy(msg.data, buffer, len);
                    msg.len = len;
                    msg.receiveTime = millis();
                    msg.rssi = radio.getRSSI();
                    msg.snr = radio.getSNR();
                    msg.isForRebroadcast = false;
                    
                    // Try to queue the message
                    if (xQueueSend(loraRxQueue, &msg, 0) != pdTRUE) {
                        queueOverflows++;
                        Serial.println("[WARNING] LoRa RX queue full, dropping message!");
                    }
                }
            }
            
            // Immediately return to receive mode
            radio.startReceive();
            xSemaphoreGive(radioMutex);
        }
    }
}

// LoRa TX Task - handles transmissions with smart collision avoidance
void loraTxTask(void* parameter) {
    QueuedMessage msg;
    uint32_t lastTransmitTime = 0;
    
    while (true) {
        if (xQueueReceive(loraTxQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
            // Smart collision avoidance
            uint32_t now = millis();
            uint32_t timeSinceLastTx = now - lastTransmitTime;
            
            // Wait until scheduled time if set
            if (msg.scheduledTransmitTime > now) {
                vTaskDelay(pdMS_TO_TICKS(msg.scheduledTransmitTime - now));
            }
            
            // Additional backoff if we're transmitting too frequently
            if (timeSinceLastTx < currentBackoff) {
                uint32_t additionalDelay = currentBackoff - timeSinceLastTx;
                vTaskDelay(pdMS_TO_TICKS(additionalDelay));
                collisionBackoffs++;
            }
            
            if (xSemaphoreTake(radioMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                int state = radio.transmit(msg.data, msg.len);
                
                if (state == RADIOLIB_ERR_NONE) {
                    loraTxCount++;
                    lastTransmitTime = millis();
                    
                    // Reduce backoff on success
                    currentBackoff = max(BASE_BACKOFF_MS, currentBackoff * 3 / 4);
                } else {
                    // Increase backoff on failure
                    currentBackoff = min(MAX_BACKOFF_MS, currentBackoff * 2);
                    Serial.printf("[ERROR] LoRa TX failed: %d, backoff now %dms\n", 
                                 state, currentBackoff);
                }
                
                // Return to receive mode
                radio.startReceive();
                xSemaphoreGive(radioMutex);
            }
        }
    }
}

// Main message processing task - handles routing logic
void messageProcessingTask(void* parameter) {
    QueuedMessage msg;
    
    while (true) {
        // Process LoRa RX messages
        if (xQueueReceive(loraRxQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
            processLoRaMessage(&msg);
        }
        
        // Process BLE RX messages
        if (xQueueReceive(bleRxQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
            processBleMessage(&msg);
        }
    }
}

// Process a received LoRa message
void processLoRaMessage(QueuedMessage* msg) {
    if (msg->len < sizeof(BridgeHdr)) return;
    
    Frame* frame = (Frame*)msg->data;
    
    // Validate magic
    if (frame->hdr.magic != BRIDGE_MAGIC) {
        return;
    }
    
    loraRxCount++;
    
    // Check if this is our own message
    bool isOurMessage = false;
    if (xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        isOurMessage = (ourMessages.find(frame->hdr.msg_id) != ourMessages.end());
        xSemaphoreGive(cacheMutex);
    }
    
    // Check for duplicates
    bool isNewMessage = false;
    if (xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        isNewMessage = !isDuplicateLocked(frame->hdr.msg_id);
        xSemaphoreGive(cacheMutex);
    }
    
    // Forward to BLE if appropriate
    if (deviceConnected && !isOurMessage && frame->hdr.payload_len > 0) {
        QueuedMessage bleMsg;
        uint8_t* payload = msg->data + sizeof(BridgeHdr) + sizeof(uint16_t);
        memcpy(bleMsg.data, payload, frame->hdr.payload_len);
        bleMsg.len = frame->hdr.payload_len;
        bleMsg.receiveTime = msg->receiveTime;
        
        if (xQueueSend(bleTxQueue, &bleMsg, 0) != pdTRUE) {
            droppedMessages++;
            Serial.println("[WARNING] BLE TX queue full!");
        }
    }
    
    // Schedule re-broadcast if appropriate
    if (isNewMessage && frame->hdr.ttl > 1 && !isOurMessage) {
        frame->hdr.ttl--;
        frame->hdr.hop++;
        
        QueuedMessage rebroadcast;
        memcpy(rebroadcast.data, msg->data, msg->len);
        rebroadcast.len = msg->len;
        rebroadcast.isForRebroadcast = true;
        
        // Smart scheduling based on RSSI and current network load
        uint32_t baseDelay = 20;  // Base delay in ms
        
        // Add delay based on signal strength (stronger signals wait longer)
        if (msg->rssi > -50) {
            baseDelay += 50;  // Very strong signal, wait longer
        } else if (msg->rssi > -70) {
            baseDelay += 30;  // Medium signal
        }
        
        // Add random jitter
        uint32_t jitter = random(0, 30);
        rebroadcast.scheduledTransmitTime = millis() + baseDelay + jitter;
        
        if (xQueueSend(loraTxQueue, &rebroadcast, 0) != pdTRUE) {
            droppedMessages++;
            Serial.println("[WARNING] LoRa TX queue full!");
        }
    }
}

// BLE TX Task - handles sending to BLE clients
void bleTask(void* parameter) {
    QueuedMessage msg;
    
    while (true) {
        if (xQueueReceive(bleTxQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (deviceConnected && pTxCharacteristic) {
                if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    pTxCharacteristic->setValue(msg.data, msg.len);
                    pTxCharacteristic->notify();
                    bleTxCount++;
                    xSemaphoreGive(bleMutex);
                }
            }
        }
    }
}

// Check if message is duplicate (for re-broadcast decision)
bool isDuplicate(uint64_t msgId) {
    cleanupMessageCache();
    
    if (messageCache.find(msgId) != messageCache.end()) {
        return true;
    }
    
    // Add to cache
    MessageInfo info = {msgId, 0, millis()};
    messageCache[msgId] = info;
    
    // Limit cache size
    while (messageCache.size() > MAX_CACHE_SIZE) {
        messageCache.erase(messageCache.begin());
    }
    
    // Clean up our messages set too
    if (ourMessages.size() > MAX_CACHE_SIZE) {
        ourMessages.clear();  // Simple cleanup
    }
    
    return false;
}

// Clean expired entries from cache
void cleanupMessageCache() {
    uint32_t now = millis();
    auto it = messageCache.begin();
    
    while (it != messageCache.end()) {
        if (now - it->second.timestamp > CACHE_TIMEOUT_MS) {
            it = messageCache.erase(it);
        } else {
            ++it;
        }
    }
}

// Update statistics
void updateStats() {
    if (pConfigCharacteristic) {
        String configData = "{"
                           "\"gwId\":" + String(gatewayId) + ","
                           "\"name\":\"" + gatewayName + "\","
                           "\"uptime\":" + String(millis()/1000) + ","
                           "\"loraRx\":" + String(loraRxCount) + ","
                           "\"loraTx\":" + String(loraTxCount) + ","
                           "\"bleRx\":" + String(bleRxCount) + ","
                           "\"bleTx\":" + String(bleTxCount) + ","
                           "\"region\":1,"
                           "\"sf\":" + String(LORA_SPREADING) + ","
                           "\"txDbm\":" + String(LORA_TX_POWER) +
                           "}";
        pConfigCharacteristic->setValue(configData.c_str());
        if (deviceConnected) {
            pConfigCharacteristic->notify();
        }
    }
}

// Process serial commands for testing
void processSerialCommand() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        
        if (cmd == "status") {
            Serial.println("\n╔═══════════════════════════════════════╗");
            Serial.printf("║ %s Status Report        ║\n", gatewayName.c_str());
            Serial.println("╠═══════════════════════════════════════╣");
            Serial.printf("║ Gateway ID: 0x%08X               ║\n", gatewayId);
            Serial.printf("║ Uptime: %lu seconds                  ║\n", millis()/1000);
            Serial.printf("║ BLE Connected: %s                    ║\n", deviceConnected ? "YES" : "NO ");
            Serial.printf("║ BLE RX/TX: %lu/%lu                   ║\n", bleRxCount, bleTxCount);
            Serial.printf("║ LoRa RX/TX: %lu/%lu                  ║\n", loraRxCount, loraTxCount);
            Serial.printf("║ Message Cache: %d entries            ║\n", messageCache.size());
            Serial.printf("║ Our Messages: %d entries             ║\n", ourMessages.size());
            Serial.printf("║ Free Memory: %d bytes                ║\n", ESP.getFreeHeap());
            Serial.println("╚═══════════════════════════════════════╝");
        } else if (cmd == "test") {
            // Send a test message
            uint8_t testData[] = "Test message from serial";
            sendLoRaMessage(testData, sizeof(testData)-1);
        } else if (cmd == "broadcast") {
            // Manually trigger status broadcast
            broadcastGatewayStatus();
        } else if (cmd == "help") {
            Serial.println("\nAvailable commands:");
            Serial.println("  status    - Show current status");
            Serial.println("  test      - Send test LoRa message");
            Serial.println("  broadcast - Send gateway status broadcast");
            Serial.println("  help      - Show this help");
        }
    }
}

void broadcastGatewayStatus() {
    // Create a status message with connected device info
    // Format: "STATUS|gateway_id|gateway_name|connected_count|nickname1,nickname2,..."
    
    String statusMsg = "STATUS|";
    statusMsg += String(gatewayId, HEX);
    statusMsg += "|";
    statusMsg += gatewayName;
    statusMsg += "|";
    statusMsg += String(pServer ? pServer->getConnectedCount() : 0);
    
    // Add connected device nicknames if any
    if (!connectedDeviceNicknames.empty()) {
        statusMsg += "|";
        for (size_t i = 0; i < connectedDeviceNicknames.size(); i++) {
            if (i > 0) statusMsg += ",";
            statusMsg += connectedDeviceNicknames[i];
        }
    }
    
    // DEBUG: Log exact message and length
    Serial.printf("\n│ DEBUG: STATUS message content: '%s'                 │\n", statusMsg.c_str());
    Serial.printf("│ DEBUG: STATUS message length: %d bytes              │\n", statusMsg.length());
    
    Serial.println("\n┌─── GATEWAY STATUS BROADCAST ─────────────────────────┐");
    Serial.printf("│ Broadcasting gateway status to mesh network          │\n");
    Serial.printf("│ Status: %s │\n", statusMsg.c_str());
    Serial.println("└──────────────────────────────────────────────────────┘");
    
    // Send the status message directly (it will be wrapped by sendLoRaMessage)
    sendLoRaMessage((uint8_t*)statusMsg.c_str(), statusMsg.length());
    
    // Also send to connected BLE clients directly
    if (deviceConnected && pTxCharacteristic) {
        pTxCharacteristic->setValue((uint8_t*)statusMsg.c_str(), statusMsg.length());
        pTxCharacteristic->notify();
        Serial.println("│ Also sent status to connected BLE client            │");
    }
}

// After the existing isDuplicate function, add the thread-safe version
bool isDuplicateLocked(uint64_t msgId) {
    // This version assumes the mutex is already held by the caller
    cleanupMessageCache();
    
    if (messageCache.find(msgId) != messageCache.end()) {
        return true;
    }
    
    // Add to cache
    MessageInfo info = {msgId, 0, millis()};
    messageCache[msgId] = info;
    
    // Limit cache size
    while (messageCache.size() > MAX_CACHE_SIZE) {
        messageCache.erase(messageCache.begin());
    }
    
    // Clean up our messages set too
    if (ourMessages.size() > MAX_CACHE_SIZE) {
        ourMessages.clear();  // Simple cleanup
    }
    
    return false;
}

// Add the processBleMessage function
void processBleMessage(QueuedMessage* msg) {
    bleRxCount++;
    
    Serial.printf("\n[%s] ━━━ BLE RX #%lu: %d bytes ━━━\n", 
                 gatewayName.c_str(), bleRxCount, msg->len);
    
    // Get message type description
    String msgType = getMessageTypeDescription(msg->data, msg->len);
    Serial.printf("Message Type: %s\n", msgType.c_str());
    
    // Try to extract nickname from BitChat message
    String messageStr((const char*)msg->data, msg->len);
    
    // Look for patterns like "nickname connected" or messages from specific senders
    if (messageStr.indexOf(" connected") > 0 && messageStr.indexOf("system") < 0) {
        int spacePos = messageStr.indexOf(" connected");
        if (spacePos > 0) {
            String nickname = messageStr.substring(0, spacePos);
            nickname.trim();
            if (nickname.length() > 0 && nickname != "system") {
                bool found = false;
                for (const auto& n : connectedDeviceNicknames) {
                    if (n == nickname) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    connectedDeviceNicknames.push_back(nickname);
                    Serial.printf("[%s] Added connected device: %s\n", 
                                gatewayName.c_str(), nickname.c_str());
                }
            }
        }
    }
    
    // Wrap in bridge header and forward to LoRa
    Frame frame;
    memset(&frame, 0, sizeof(Frame));
    
    frame.hdr.magic = BRIDGE_MAGIC;
    frame.hdr.ver = 1;
    frame.hdr.ttl = defaultTTL;
    frame.hdr.msg_id = ((uint64_t)random(0xFFFFFFFF) << 32) | millis();
    frame.hdr.gw_id = gatewayId;
    frame.hdr.frag_idx = 0;
    frame.hdr.frag_total = 1;
    frame.hdr.payload_len = msg->len;
    frame.hdr.hop = 0;
    frame.hdr.crc16 = 0;
    
    frame.len = msg->len;
    memcpy(frame.data, msg->data, msg->len);
    
    // Track that we sent this message
    if (xSemaphoreTake(cacheMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        ourMessages.insert(frame.hdr.msg_id);
        
        // Add to cache
        MessageInfo info = {frame.hdr.msg_id, gatewayId, millis()};
        messageCache[frame.hdr.msg_id] = info;
        xSemaphoreGive(cacheMutex);
    }
    
    // Queue for transmission
    QueuedMessage loraMsg;
    size_t totalLen = sizeof(BridgeHdr) + sizeof(uint16_t) + frame.len;
    memcpy(loraMsg.data, &frame, totalLen);
    loraMsg.len = totalLen;
    loraMsg.receiveTime = millis();
    loraMsg.scheduledTransmitTime = millis();  // Send immediately
    
    if (xQueueSend(loraTxQueue, &loraMsg, 0) != pdTRUE) {
        droppedMessages++;
        Serial.println("[WARNING] LoRa TX queue full!");
    } else {
        Serial.println("┌─── BLE → LoRa QUEUED ────────────────────────────────┐");
        Serial.printf("│ Message ID: 0x%016llX                       │\n", frame.hdr.msg_id);
        Serial.printf("│ Payload Size: %d bytes                              │\n", msg->len);
        Serial.printf("│ Message Type: %s                                     │\n", msgType.c_str());
        Serial.println("└──────────────────────────────────────────────────────┘");
    }
    
    // Send echo response
    if (pTxCharacteristic) {
        String response = "Echo: " + String(bleRxCount);
        QueuedMessage echoMsg;
        memcpy(echoMsg.data, response.c_str(), response.length());
        echoMsg.len = response.length();
        
        if (xQueueSend(bleTxQueue, &echoMsg, 0) == pdTRUE) {
            Serial.printf("Queued echo response: '%s'\n", response.c_str());
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    // Generate unique gateway ID from MAC address
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    gatewayId = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];
    
    // Determine device number based on last byte of MAC
    deviceNumber = mac[5] & 0x0F;
    
    // Create human-readable name
    char nameBuffer[32];
    snprintf(nameBuffer, sizeof(nameBuffer), "Repeater-%d", deviceNumber);
    gatewayName = String(nameBuffer);
    
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║   BitChat LoRa Gateway Repeater        ║");
    Serial.println("╚════════════════════════════════════════╝");
    Serial.printf("Device Name: %s\n", gatewayName.c_str());
    Serial.printf("Gateway ID: 0x%08X\n", gatewayId);
    Serial.printf("MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    // Initialize LoRa radio
    Serial.print("Initializing LoRa radio... ");
    
    // Set SPI pins explicitly for Heltec
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    
    int state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING, 
                           LORA_CODING_RATE, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 
                           LORA_TX_POWER, LORA_PREAMBLE);
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("success!");
        
        // Set interrupt for receive
        radio.setDio1Action(onLoRaReceive);
        
        // Start receiving
        state = radio.startReceive();
        if (state != RADIOLIB_ERR_NONE) {
            Serial.printf("startReceive failed: %d\n", state);
        }
    } else {
        Serial.printf("failed, code %d\n", state);
    }
    
    // Initialize BLE with unique name
    String bleName = "BC-" + gatewayName;
    NimBLEDevice::init(bleName.c_str());
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    
    // Create BLE Server
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    // Create BLE Service
    pService = pServer->createService(GATEWAY_SERVICE_UUID);
    
    // Create characteristics
    pTxCharacteristic = pService->createCharacteristic(
        GATEWAY_TX_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );
    
    pRxCharacteristic = pService->createCharacteristic(
        GATEWAY_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new ImprovedRxCallbacks());
    
    pConfigCharacteristic = pService->createCharacteristic(
        GATEWAY_CONFIG_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    
    // Set initial config
    updateStats();
    
    // Start service
    pService->start();
    
    // Setup advertising
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(GATEWAY_SERVICE_UUID);
    pAdvertising->setMinInterval(32);
    pAdvertising->setMaxInterval(64);
    pAdvertising->setScanResponse(false);
    
    // Add service data
    uint8_t serviceData[4] = {
        0x01,        // Version
        0x07,        // Capabilities
        0x00, 0x00   // Reserved
    };
    pAdvertising->setServiceData(NimBLEUUID(GATEWAY_SERVICE_UUID), 
                                std::string((char*)serviceData, 4));
    
    // Start advertising
    pAdvertising->start();
    
    Serial.println("*** BLE Server Started Successfully ***");
    Serial.printf("Service UUID: %s\n", GATEWAY_SERVICE_UUID);
    Serial.printf("Device advertising as '%s'\n", bleName.c_str());
    Serial.printf("LoRa: %.1f MHz, SF%d, BW%.0f kHz\n", 
                  LORA_FREQUENCY, LORA_SPREADING, LORA_BANDWIDTH);
    Serial.println("=== Gateway Ready ===");
    Serial.println("Type 'help' for available commands\n");
    
    updateStats();

    // Create queues
    loraRxQueue = xQueueCreate(20, sizeof(QueuedMessage));
    loraTxQueue = xQueueCreate(20, sizeof(QueuedMessage));
    bleRxQueue = xQueueCreate(10, sizeof(QueuedMessage));
    bleTxQueue = xQueueCreate(10, sizeof(QueuedMessage));
    
    // Create semaphores
    radioMutex = xSemaphoreCreateMutex();
    bleMutex = xSemaphoreCreateMutex();
    cacheMutex = xSemaphoreCreateMutex();
    
    // Create tasks on different cores for better performance
    xTaskCreatePinnedToCore(
        loraRxTask,
        "LoRa RX",
        4096,
        NULL,
        2,  // High priority
        &loraRxTaskHandle,
        0   // Core 0
    );
    
    xTaskCreatePinnedToCore(
        loraTxTask,
        "LoRa TX",
        4096,
        NULL,
        1,  // Medium priority
        &loraTxTaskHandle,
        0   // Core 0
    );
    
    xTaskCreatePinnedToCore(
        messageProcessingTask,
        "Message Processing",
        8192,
        NULL,
        1,  // Medium priority
        NULL,
        1   // Core 1
    );
    
    xTaskCreatePinnedToCore(
        bleTask,
        "BLE Handler",
        4096,
        NULL,
        1,  // Medium priority
        NULL,
        1   // Core 1
    );
    
    Serial.println("=== Gateway Ready with Improved Message Handling ===");
}

// Simplified main loop
void loop() {
    // Process serial commands
    processSerialCommand();
    
    // Periodic status with performance metrics
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 15000) {
        lastStatus = millis();
        
        Serial.printf("\n[%s] === Enhanced Status Update ===\n", gatewayName.c_str());
        Serial.printf("Uptime: %lu s\n", millis()/1000);
        Serial.printf("BLE: RX=%lu TX=%lu Connected=%d\n", 
                     bleRxCount, bleTxCount, deviceConnected);
        Serial.printf("LoRa: RX=%lu TX=%lu\n", loraRxCount, loraTxCount);
        Serial.printf("Performance: Dropped=%lu QueueOverflows=%lu Backoffs=%lu\n",
                     droppedMessages, queueOverflows, collisionBackoffs);
        Serial.printf("Current Backoff: %dms\n", currentBackoff);
        Serial.printf("Queue Status: LoRaRX=%d/%d LoRaTX=%d/%d\n",
                     uxQueueMessagesWaiting(loraRxQueue), 20,
                     uxQueueMessagesWaiting(loraTxQueue), 20);
        Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    }
    
    // Send periodic heartbeat if connected
    static uint32_t lastHeartbeat = 0;
    if (deviceConnected && pTxCharacteristic && millis() - lastHeartbeat > 60000) {
        lastHeartbeat = millis();
        String heartbeat = "Heartbeat: " + String(millis()/1000) + "s uptime";
        pTxCharacteristic->setValue(heartbeat.c_str());
        pTxCharacteristic->notify();
        Serial.printf("Sent heartbeat: '%s'\n", heartbeat.c_str());
    }

    // Periodically broadcast gateway status
    if (deviceConnected && pServer && millis() - lastStatusBroadcast > STATUS_BROADCAST_INTERVAL) {
        lastStatusBroadcast = millis();
        broadcastGatewayStatus();
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
}