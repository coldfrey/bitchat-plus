// ESP32-S3 LoRa Gateway Repeater for BitChat
// Acts as a BLE↔LoRa bridge to extend BitChat range through a mesh network
// Forwards BitChat frames between BLE and LoRa with TTL and deduplication

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <RadioLib.h>
#include <map>
#include <set>  // Add this include
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

// Forward declarations
void sendLoRaMessage(const uint8_t* data, size_t len);
void cleanupMessageCache();
bool isDuplicate(uint64_t msgId);
void updateStats();
void onLoRaReceive();
void processLoRaReceive();
void processSerialCommand();

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
            Serial.printf("[%s] *** BLE RX #%lu: %d bytes ***\n", 
                         gatewayName.c_str(), bleRxCount, rxValue.length());
            
            // Print hex dump of received data
            Serial.print("Data: ");
            for(int i = 0; i < rxValue.length(); i++) {
                Serial.printf("%02X ", (uint8_t)rxValue[i]);
            }
            Serial.println();
            
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

// Send message over LoRa
void sendLoRaMessage(const uint8_t* data, size_t len) {
    // Check packet size limit
    if (len > 200) {
        Serial.printf("[%s] ERROR: Packet too long (%d bytes), max 200\n", 
                     gatewayName.c_str(), len);
        return;
    }
    
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
    
    // Track that we sent this message
    ourMessages.insert(frame.hdr.msg_id);
    
    // Add to cache
    MessageInfo info = {frame.hdr.msg_id, gatewayId, millis()};
    messageCache[frame.hdr.msg_id] = info;
    
    // Transmit over LoRa
    size_t totalLen = sizeof(BridgeHdr) + frame.len;
    
    Serial.println("┌─── BLE → LoRa ───────────────────────┐");
    Serial.printf("│ Received %d bytes from iOS app       │\n", len);
    Serial.printf("│ Message ID: 0x%016llX       │\n", frame.hdr.msg_id);
    Serial.printf("│ Our Gateway: 0x%08X             │\n", gatewayId);
    Serial.printf("│ TTL: %d, Hop: %d                    │\n", frame.hdr.ttl, frame.hdr.hop);
    Serial.println("└──────────────────────────────────────┘");
    
    Serial.printf("[%s] LoRa TX: %d bytes, TTL=%d, msg_id=%llx\n", 
                 gatewayName.c_str(), totalLen, frame.hdr.ttl, frame.hdr.msg_id);
    
    int state = radio.transmit((uint8_t*)&frame, totalLen);
    
    if (state == RADIOLIB_ERR_NONE) {
        loraTxCount++;
        Serial.printf("[%s] LoRa TX success!\n", gatewayName.c_str());
    } else {
        Serial.printf("[%s] LoRa TX failed: %d\n", gatewayName.c_str(), state);
    }
    
    // Return to receive mode
    radio.startReceive();
    updateStats();
}

// LoRa interrupt handler
void onLoRaReceive() {
    loraRxFlag = true;
}

// Process received LoRa messages
void processLoRaReceive() {
    if (!loraRxFlag) return;
    loraRxFlag = false;
    
    uint8_t buffer[256];
    size_t len = radio.getPacketLength();
    
    if (len == 0) return;
    
    int state = radio.readData(buffer, len);
    
    if (state == RADIOLIB_ERR_NONE && len >= sizeof(BridgeHdr)) {
        loraRxCount++;
        Frame* frame = (Frame*)buffer;
        
        Serial.println("\n┌─── LoRa Message Received ────────────┐");
        Serial.printf("│ From Gateway: 0x%08X            │\n", frame->hdr.gw_id);
        Serial.printf("│ Message ID: 0x%016llX       │\n", frame->hdr.msg_id);
        Serial.printf("│ TTL: %d, Hop: %d                    │\n", frame->hdr.ttl, frame->hdr.hop);
        Serial.printf("│ Payload: %d bytes                   │\n", frame->hdr.payload_len);
        
        if (frame->hdr.magic != BRIDGE_MAGIC) {
            Serial.println("│ Status: Invalid magic number        │");
            Serial.println("└──────────────────────────────────────┘");
            radio.startReceive();
            return;
        }
        
        // Check if this is our own message echoing back
        bool isOurMessage = (ourMessages.find(frame->hdr.msg_id) != ourMessages.end());
        
        // Check if we've seen this message before (for re-broadcast decision)
        bool isNewMessage = !isDuplicate(frame->hdr.msg_id);
        
        // IMPORTANT: Forward to BLE ONLY if:
        // 1. We have a connected client
        // 2. This is NOT a message we originated (no echo back to app)
        if (deviceConnected && pTxCharacteristic && frame->hdr.payload_len > 0 && !isOurMessage) {
            // Extract the BitChat payload
            uint8_t* payload = buffer + sizeof(BridgeHdr);
            size_t payloadLen = frame->hdr.payload_len;
            
            // Forward to BLE
            pTxCharacteristic->setValue(payload, payloadLen);
            pTxCharacteristic->notify();
            bleTxCount++;
            
            Serial.println("│ Action: FORWARDED TO BLE CLIENT ✓   │");
            Serial.printf("│ BLE TX: %d bytes sent              │\n", payloadLen);
            
            // Show first few bytes of payload
            Serial.print("│ Data: ");
            for (int i = 0; i < min(payloadLen, (size_t)8); i++) {
                Serial.printf("%02X ", payload[i]);
            }
            if (payloadLen > 8) Serial.print("...");
            Serial.println("        │");
        } else if (isOurMessage) {
            Serial.println("│ Status: Our own message (no BLE TX) │");
        } else if (!deviceConnected) {
            Serial.println("│ Status: No BLE client connected     │");
        }
        
        // Re-broadcast decision (mesh networking)
        if (isNewMessage && frame->hdr.ttl > 1 && !isOurMessage) {
            Serial.println("│ Action: Re-broadcasting (mesh)      │");
            
            frame->hdr.ttl--;
            frame->hdr.hop++;
            
            // Add random delay to avoid collisions
            int delayMs = random(50, 200);
            Serial.printf("│ Delay: %d ms (collision avoid)     │\n", delayMs);
            delay(delayMs);
            
            int state = radio.transmit(buffer, len);
            if (state == RADIOLIB_ERR_NONE) {
                loraTxCount++;
                Serial.println("│ Re-broadcast: SUCCESS ✓             │");
            } else {
                Serial.printf("│ Re-broadcast: FAILED (err %d)      │\n", state);
            }
        } else if (!isNewMessage) {
            Serial.println("│ Status: Duplicate (no rebroadcast)  │");
        } else if (frame->hdr.ttl <= 1) {
            Serial.println("│ Status: TTL expired (no rebroadcast)│");
        } else if (isOurMessage) {
            Serial.println("│ Status: Our message (no rebroadcast)│");
        }
        
        Serial.println("└──────────────────────────────────────┘");
        updateStats();
    }
    
    // Restart receive mode
    radio.startReceive();
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
        } else if (cmd == "help") {
            Serial.println("\nAvailable commands:");
            Serial.println("  status - Show current status");
            Serial.println("  test   - Send test LoRa message");
            Serial.println("  help   - Show this help");
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
    pRxCharacteristic->setCallbacks(new MyRxCallbacks());
    
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
}

void loop() {
    // Process LoRa receives
    processLoRaReceive();
    
    // Process serial commands
    processSerialCommand();
    
    // Periodic status
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 15000) {
        lastStatus = millis();
        
        Serial.printf("\n[%s] === Status Update ===\n", gatewayName.c_str());
        Serial.printf("Uptime: %lu s\n", millis()/1000);
        Serial.printf("BLE: RX=%lu TX=%lu Connected=%d\n", 
                     bleRxCount, bleTxCount, deviceConnected);
        Serial.printf("LoRa: RX=%lu TX=%lu\n", loraRxCount, loraTxCount);
        Serial.printf("Cache size: %d messages\n", messageCache.size());
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
    
    delay(10);
}