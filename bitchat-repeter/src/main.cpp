#include <Arduino.h>
#include <NimBLEDevice.h>

// Enhanced Gateway BLE server with proper service and characteristics
// This creates a full BLE server that iOS can discover, connect to, and communicate with

#define GATEWAY_SERVICE_UUID     "7A1B0000-6B2E-46E8-8D2D-6AA2E5A4F001"
#define GATEWAY_TX_CHAR_UUID     "7A1B0001-6B2E-46E8-8D2D-6AA2E5A4F001"  // ESP32 -> iOS
#define GATEWAY_RX_CHAR_UUID     "7A1B0002-6B2E-46E8-8D2D-6AA2E5A4F001"  // iOS -> ESP32
#define GATEWAY_CONFIG_CHAR_UUID "7A1B0003-6B2E-46E8-8D2D-6AA2E5A4F001"  // Config/Stats

NimBLEServer* pServer = nullptr;
NimBLEService* pService = nullptr;
NimBLECharacteristic* pTxCharacteristic = nullptr;
NimBLECharacteristic* pRxCharacteristic = nullptr;
NimBLECharacteristic* pConfigCharacteristic = nullptr;
NimBLEAdvertising* pAdvertising = nullptr;

bool deviceConnected = false;
uint32_t messageCount = 0;

// BLE Server Callbacks
class MyServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) {
        deviceConnected = true;
        Serial.println("*** BLE Client Connected ***");
        Serial.printf("Connected clients: %d\n", pServer->getConnectedCount());
    }

    void onDisconnect(NimBLEServer* pServer) {
        deviceConnected = false;
        Serial.println("*** BLE Client Disconnected ***");
        Serial.printf("Connected clients: %d\n", pServer->getConnectedCount());
        
        // Restart advertising
        pServer->startAdvertising();
        Serial.println("BLE advertising restarted");
    }
};

// RX Characteristic Callback (receives data from iOS)
class MyRxCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) {
        std::string rxValue = pCharacteristic->getValue();
        
        if (rxValue.length() > 0) {
            messageCount++;
            Serial.printf("*** Received message #%lu ***\n", messageCount);
            Serial.printf("Length: %d bytes\n", rxValue.length());
            Serial.print("Data: ");
            for(int i = 0; i < rxValue.length(); i++) {
                Serial.printf("%02X ", (uint8_t)rxValue[i]);
            }
            Serial.println();
            
            // Try to decode as text
            if (rxValue.length() > 0) {
                String textData = "";
                bool isPrintable = true;
                for(int i = 0; i < rxValue.length(); i++) {
                    char c = rxValue[i];
                    if (c >= 32 && c <= 126) {
                        textData += c;
                    } else if (c == 0) {
                        // Null terminator, stop here
                        break;
                    } else {
                        isPrintable = false;
                        break;
                    }
                }
                if (isPrintable && textData.length() > 0) {
                    Serial.printf("As text: '%s'\n", textData.c_str());
                }
            }
            
            // Echo back to TX characteristic
            if (pTxCharacteristic) {
                String response = "Echo: " + String(messageCount);
                pTxCharacteristic->setValue(response.c_str());
                pTxCharacteristic->notify();
                Serial.printf("Sent echo response: '%s'\n", response.c_str());
            }
        }
    }
};

void setup() {
    Serial.begin(115200);
    delay(2000);  // Give time for serial to initialize
    
    Serial.println("\n=== BitChat Enhanced Gateway BLE Server ===");
    Serial.println("Initializing BLE server with full service...");
    
    // Initialize NimBLE
    NimBLEDevice::init("BC-Gateway");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Maximum power
    
    // Create BLE Server
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    // Create BLE Service
    pService = pServer->createService(GATEWAY_SERVICE_UUID);
    
    // Create TX Characteristic (ESP32 -> iOS, with notify)
    pTxCharacteristic = pService->createCharacteristic(
        GATEWAY_TX_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );
    
    // Create RX Characteristic (iOS -> ESP32, with write)
    pRxCharacteristic = pService->createCharacteristic(
        GATEWAY_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new MyRxCallbacks());
    
    // Create Config/Stats Characteristic (readable config and stats)
    pConfigCharacteristic = pService->createCharacteristic(
        GATEWAY_CONFIG_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    
    // Set initial config data (mock gateway stats)
    String configData = "{"
                       "\"gwId\":305419896,"
                       "\"uptime\":0,"
                       "\"loraRx\":0,"
                       "\"loraTx\":0,"
                       "\"bleRx\":0,"
                       "\"bleTx\":0,"
                       "\"region\":1,"
                       "\"sf\":7,"
                       "\"txDbm\":14"
                       "}";
    pConfigCharacteristic->setValue(configData.c_str());
    
    // Start the service
    pService->start();
    
    // Setup advertising
    pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(GATEWAY_SERVICE_UUID);
    pAdvertising->setMinInterval(32);  // 20ms
    pAdvertising->setMaxInterval(64);  // 40ms
    pAdvertising->setScanResponse(false);
    
    // Add service data with version and capabilities
    uint8_t serviceData[4] = {
        0x01,        // Version
        0x07,        // Capabilities: BLE+LoRa+Stats
        0x00, 0x00   // Reserved
    };
    pAdvertising->setServiceData(NimBLEUUID(GATEWAY_SERVICE_UUID), 
                                std::string((char*)serviceData, 4));
    
    // Start advertising
    pAdvertising->start();
    
    Serial.println("*** BLE Server Started Successfully ***");
    Serial.printf("Service UUID: %s\n", GATEWAY_SERVICE_UUID);
    Serial.printf("TX Char UUID: %s\n", GATEWAY_TX_CHAR_UUID);
    Serial.printf("RX Char UUID: %s\n", GATEWAY_RX_CHAR_UUID);
    Serial.printf("Config Char UUID: %s\n", GATEWAY_CONFIG_CHAR_UUID);
    Serial.println("Device advertising as 'BC-Gateway'");
    Serial.println("Waiting for iOS app to connect...");
    Serial.println("=== BLE Server Ready ===\n");
}

void loop() {
    static uint32_t lastPrint = 0;
    static uint32_t lastStatsUpdate = 0;
    uint32_t now = millis();
    
    // Print status every 15 seconds
    if (now - lastPrint > 15000) {
        lastPrint = now;
        Serial.printf("=== Status Update (Uptime: %lu s) ===\n", now/1000);
        Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
        Serial.printf("Connected clients: %d\n", pServer ? pServer->getConnectedCount() : 0);
        Serial.printf("Messages received: %lu\n", messageCount);
        
        if (deviceConnected) {
            Serial.println("BLE Client is connected and active");
        } else {
            Serial.println("No clients connected, still advertising...");
        }
        Serial.println();
    }
    
    // Update config stats every 30 seconds
    if (now - lastStatsUpdate > 30000) {
        lastStatsUpdate = now;
        
        if (pConfigCharacteristic && deviceConnected) {
            // Update stats in config characteristic
            String configData = "{"
                               "\"gwId\":305419896,"
                               "\"uptime\":" + String(now/1000) + ","
                               "\"loraRx\":0,"
                               "\"loraTx\":0,"
                               "\"bleRx\":" + String(messageCount) + ","
                               "\"bleTx\":" + String(messageCount) + ","
                               "\"region\":1,"
                               "\"sf\":7,"
                               "\"txDbm\":14"
                               "}";
            pConfigCharacteristic->setValue(configData.c_str());
            pConfigCharacteristic->notify();
            Serial.println("Updated config/stats and notified client");
        }
    }
    
    // Send periodic heartbeat if connected
    static uint32_t lastHeartbeat = 0;
    if (deviceConnected && pTxCharacteristic && now - lastHeartbeat > 60000) {
        lastHeartbeat = now;
        String heartbeat = "Heartbeat: " + String(now/1000) + "s uptime";
        pTxCharacteristic->setValue(heartbeat.c_str());
        pTxCharacteristic->notify();
        Serial.printf("Sent heartbeat: '%s'\n", heartbeat.c_str());
    }
    
    delay(1000);
}