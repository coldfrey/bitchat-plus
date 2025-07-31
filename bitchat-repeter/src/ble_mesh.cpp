#include "ble_mesh.h"
#include <Arduino.h>
#include <WiFi.h>

// Static member definitions
NimBLEServer* BLEMesh::pServer = nullptr;
NimBLEService* BLEMesh::pService = nullptr;
NimBLECharacteristic* BLEMesh::pCharacteristic = nullptr;
NimBLEAdvertising* BLEMesh::pAdvertising = nullptr;
String BLEMesh::myPeerID = "";

void BLEMesh::init() {
    Serial.println("BLE Mesh: Initializing...");
    
    // Generate peer ID from MAC address
    generatePeerID();
    
    // Initialize NimBLE
    NimBLEDevice::init("BitChat-Repeater");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Maximum power
    
    // Create BLE Server
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());
    
    // Create BitChat service
    pService = pServer->createService(BITCHAT_SERVICE_UUID);
    
    // Create characteristic with read, write, and notify properties
    pCharacteristic = pService->createCharacteristic(
        BITCHAT_CHAR_UUID,
        NIMBLE_PROPERTY::READ | 
        NIMBLE_PROPERTY::WRITE | 
        NIMBLE_PROPERTY::WRITE_NR | 
        NIMBLE_PROPERTY::NOTIFY
    );
    
    pCharacteristic->setCallbacks(new CharacteristicCallbacks());
    
    // Start the service
    pService->start();
    
    // Start advertising
    startAdvertising();
    
    Serial.printf("BLE Mesh: Initialized as peer ID: %s\n", myPeerID.c_str());
    Serial.printf("BLE Mesh: Advertising BitChat service for iOS devices: %s\n", BITCHAT_SERVICE_UUID);
}

void BLEMesh::process() {
    // NimBLE handles most processing automatically
    // This can be used for periodic tasks or maintenance
    static unsigned long lastUpdate = 0;
    unsigned long now = millis();
    
    if (now - lastUpdate > 10000) { // Every 10 seconds
        lastUpdate = now;
        
        // Check if we need to restart advertising
        if (!pServer->getConnectedCount() && !pAdvertising->isAdvertising()) {
            Serial.println("BLE Mesh: Restarting advertising");
            startAdvertising();
        }
        
        Serial.printf("BLE Mesh: Connected iOS devices: %d\n", pServer->getConnectedCount());
    }
}

String BLEMesh::getPeerID() {
    return myPeerID;
}

void BLEMesh::sendData(const uint8_t* data, size_t length) {
    // Send to all connected iOS devices
    if (pCharacteristic && pServer->getConnectedCount() > 0) {
        pCharacteristic->setValue(data, length);
        pCharacteristic->notify();
        Serial.printf("BLE Mesh: Sent %d bytes to %d connected iOS device(s)\n", 
                     length, pServer->getConnectedCount());
    } else {
        Serial.printf("BLE Mesh: No iOS devices connected to send %d bytes\n", length);
    }
}

int BLEMesh::getConnectedClientCount() {
    return pServer->getConnectedCount();
}

void BLEMesh::generatePeerID() {
    // Get MAC address and create 8-character hex peer ID
    uint8_t mac[6];
    WiFi.macAddress(mac);
    
    // Use first 4 bytes of MAC for peer ID (8 hex characters)
    char peerBuffer[9];
    snprintf(peerBuffer, sizeof(peerBuffer), "%02X%02X%02X%02X", 
             mac[0], mac[1], mac[2], mac[3]);
    
    myPeerID = String(peerBuffer);
}

void BLEMesh::startAdvertising() {
    pAdvertising = NimBLEDevice::getAdvertising();
    
    // Add service UUID to advertisement
    pAdvertising->addServiceUUID(BITCHAT_SERVICE_UUID);
    
    // Set the peer ID as the device name (this appears in iOS scan results)
    pAdvertising->setName(myPeerID.c_str());
    
    // Configure advertising parameters
    pAdvertising->setScanResponse(false);
    pAdvertising->setMinPreferred(0x0);  // Set value to 0x00 to not advertise this parameter
    
    // Start advertising
    pAdvertising->start();
    
    Serial.println("BLE Mesh: Started advertising");
}


void BLEMesh::handleReceivedData(const uint8_t* data, size_t length) {
    Serial.printf("BLE Mesh: Received %d bytes from iOS device\n", length);
    
    // Print first few bytes for debugging
    Serial.print("BLE Mesh: Data: ");
    for (size_t i = 0; i < min(length, (size_t)16); i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
    
    // TODO: Forward to message router for processing
    // MessageRouter::handleBLEMessage(data, length);
}

// Server callback implementations
void ServerCallbacks::onConnect(NimBLEServer* pServer) {
    Serial.printf("BLE Mesh: iOS device connected (total: %d)\n", pServer->getConnectedCount());
    
    // Don't stop advertising - allow multiple iOS devices to connect
}

void ServerCallbacks::onDisconnect(NimBLEServer* pServer) {
    Serial.printf("BLE Mesh: iOS device disconnected (remaining: %d)\n", pServer->getConnectedCount());
    
    // Restart advertising if no clients connected
    if (pServer->getConnectedCount() == 0) {
        delay(500); // Give it a moment
        pServer->startAdvertising();
        Serial.println("BLE Mesh: Restarted advertising after disconnect");
    }
}

// Characteristic callback implementations
void CharacteristicCallbacks::onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    
    if (value.length() > 0) {
        BLEMesh::handleReceivedData((const uint8_t*)value.data(), value.length());
    }
}