#include "ble_mesh.h"
#include <Arduino.h>
#include <WiFi.h>

// Static member definitions
NimBLEServer* BLEMesh::pServer = nullptr;
NimBLEService* BLEMesh::pService = nullptr;
NimBLECharacteristic* BLEMesh::pCharacteristic = nullptr;
NimBLEAdvertising* BLEMesh::pAdvertising = nullptr;
NimBLEScan* BLEMesh::pScan = nullptr;
std::map<String, ConnectedPeer> BLEMesh::connectedPeers;
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
    
    // Initialize scanning
    pScan = NimBLEDevice::getScan();
    pScan->setInterval(1349);
    pScan->setWindow(449);
    pScan->setActiveScan(true);
    
    // Start scanning for other BitChat devices
    startScanning();
    
    Serial.printf("BLE Mesh: Initialized as peer ID: %s\n", myPeerID.c_str());
    Serial.printf("BLE Mesh: Advertising BitChat service: %s\n", BITCHAT_SERVICE_UUID);
    Serial.println("BLE Mesh: Scanning for BitChat devices...");
}

void BLEMesh::process() {
    // NimBLE handles most processing automatically
    // This can be used for periodic tasks or maintenance
    static unsigned long lastUpdate = 0;
    static unsigned long lastScanRestart = 0;
    unsigned long now = millis();
    
    if (now - lastUpdate > 10000) { // Every 10 seconds
        lastUpdate = now;
        
        // Check if we need to restart advertising
        if (!pServer->getConnectedCount() && !pAdvertising->isAdvertising()) {
            Serial.println("BLE Mesh: Restarting advertising");
            startAdvertising();
        }
        
        // Cleanup disconnected peers
        cleanupDisconnectedPeers();
        
        Serial.printf("BLE Mesh: Server connections: %d, Client connections: %d\n", 
                     pServer->getConnectedCount(), connectedPeers.size());
    }
    
    // Restart scanning periodically if not at max connections
    if (now - lastScanRestart > 30000 && connectedPeers.size() < MAX_CONNECTIONS) { // Every 30 seconds
        lastScanRestart = now;
        if (pScan && !pScan->isScanning()) {
            Serial.println("BLE Mesh: Restarting scan");
            startScanning();
        }
    }
}

String BLEMesh::getPeerID() {
    return myPeerID;
}

void BLEMesh::sendData(const uint8_t* data, size_t length, const String& targetPeerID) {
    bool sent = false;
    
    // Send to server clients (devices connected to us as peripherals)
    if (pCharacteristic && pServer->getConnectedCount() > 0) {
        pCharacteristic->setValue(data, length);
        pCharacteristic->notify();
        sent = true;
        Serial.printf("BLE Mesh: Sent %d bytes to %d server client(s)\n", 
                     length, pServer->getConnectedCount());
    }
    
    // Send to client connections (devices we connected to as central)
    for (auto& pair : connectedPeers) {
        ConnectedPeer& peer = pair.second;
        if (peer.isReady && peer.characteristic) {
            if (targetPeerID.length() == 0 || peer.peerID == targetPeerID) {
                if (peer.characteristic->writeValue(data, length, false)) { // writeValue with response=false
                    Serial.printf("BLE Mesh: Sent %d bytes to peer %s\n", length, peer.peerID.c_str());
                    sent = true;
                } else {
                    Serial.printf("BLE Mesh: Failed to send to peer %s\n", peer.peerID.c_str());
                }
            }
        }
    }
    
    if (!sent) {
        Serial.printf("BLE Mesh: No connections available to send %d bytes\n", length);
    }
}

int BLEMesh::getConnectedPeerCount() {
    return pServer->getConnectedCount() + connectedPeers.size();
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

void BLEMesh::startScanning() {
    if (pScan && connectedPeers.size() < MAX_CONNECTIONS) {
        NimBLEScanResults results = pScan->start(10, false); // Scan for 10 seconds, don't continue previous scan
        Serial.println("BLE Mesh: Started scanning for BitChat devices");
        
        // Process scan results
        for (int i = 0; i < results.getCount(); i++) {
            NimBLEAdvertisedDevice device = results.getDevice(i);
            BLECallbacks::scanResult(&device);
        }
    }
}

void BLEMesh::stopScanning() {
    if (pScan && pScan->isScanning()) {
        pScan->stop();
        Serial.println("BLE Mesh: Stopped scanning");
    }
}

bool BLEMesh::connectToPeer(NimBLEAdvertisedDevice* device) {
    String address = device->getAddress().toString().c_str();
    
    // Check if already connected
    if (connectedPeers.find(address) != connectedPeers.end()) {
        return false;
    }
    
    // Check if at max connections
    if (connectedPeers.size() >= MAX_CONNECTIONS) {
        Serial.println("BLE Mesh: At maximum connections, not connecting");
        return false;
    }
    
    Serial.printf("BLE Mesh: Attempting to connect to %s\n", address.c_str());
    
    NimBLEClient* pClient = NimBLEDevice::createClient();
    pClient->setConnectionParams(12, 12, 0, 51); // Optimal for BitChat
    pClient->setConnectTimeout(5); // 5 second timeout
    
    if (!pClient->connect(device)) {
        Serial.printf("BLE Mesh: Failed to connect to %s\n", address.c_str());
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    // Get the service
    NimBLERemoteService* pRemoteService = pClient->getService(BITCHAT_SERVICE_UUID);
    if (!pRemoteService) {
        Serial.printf("BLE Mesh: BitChat service not found on %s\n", address.c_str());
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    // Get the characteristic
    NimBLERemoteCharacteristic* pRemoteCharacteristic = pRemoteService->getCharacteristic(BITCHAT_CHAR_UUID);
    if (!pRemoteCharacteristic) {
        Serial.printf("BLE Mesh: BitChat characteristic not found on %s\n", address.c_str());
        pClient->disconnect();
        NimBLEDevice::deleteClient(pClient);
        return false;
    }
    
    // Subscribe to notifications using function callback
    if (pRemoteCharacteristic->canNotify()) {
        pRemoteCharacteristic->subscribe(true, BLECallbacks::characteristicNotify);
    }
    
    // Store peer info
    ConnectedPeer peer;
    peer.client = pClient;
    peer.characteristic = pRemoteCharacteristic;
    peer.peerID = device->getName().c_str(); // Use advertised name as peer ID
    peer.lastSeen = millis();
    peer.isReady = true;
    
    connectedPeers[address] = peer;
    
    Serial.printf("BLE Mesh: Connected to peer %s (%s)\n", peer.peerID.c_str(), address.c_str());
    return true;
}

void BLEMesh::disconnectPeer(const String& address) {
    auto it = connectedPeers.find(address);
    if (it != connectedPeers.end()) {
        ConnectedPeer& peer = it->second;
        if (peer.client) {
            peer.client->disconnect();
            NimBLEDevice::deleteClient(peer.client);
        }
        connectedPeers.erase(it);
        Serial.printf("BLE Mesh: Disconnected from peer %s\n", address.c_str());
    }
}

void BLEMesh::cleanupDisconnectedPeers() {
    std::vector<String> toRemove;
    
    for (auto& pair : connectedPeers) {
        const String& address = pair.first;
        ConnectedPeer& peer = pair.second;
        
        if (!peer.client || !peer.client->isConnected()) {
            toRemove.push_back(address);
        }
    }
    
    for (const String& address : toRemove) {
        disconnectPeer(address);
    }
}

void BLEMesh::handleReceivedData(const uint8_t* data, size_t length, const String& sourcePeerID) {
    Serial.printf("BLE Mesh: Received %d bytes", length);
    if (sourcePeerID.length() > 0) {
        Serial.printf(" from %s", sourcePeerID.c_str());
    }
    Serial.println();
    
    // Print first few bytes for debugging
    Serial.print("BLE Mesh: Data: ");
    for (size_t i = 0; i < min(length, (size_t)16); i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
    
    // TODO: Forward to message router for processing
    // MessageRouter::handleBLEMessage(data, length, sourcePeerID);
}

// Server callback implementations
void ServerCallbacks::onConnect(NimBLEServer* pServer) {
    Serial.printf("BLE Mesh: Client connected (total: %d)\n", pServer->getConnectedCount());
    
    // Don't stop advertising - allow multiple connections
    // iOS app may connect multiple times or other repeaters may connect
}

void ServerCallbacks::onDisconnect(NimBLEServer* pServer) {
    Serial.printf("BLE Mesh: Client disconnected (remaining: %d)\n", pServer->getConnectedCount());
    
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
        BLEMesh::handleReceivedData((const uint8_t*)value.data(), value.length(), "server_client");
    }
}

// Static callback implementations
void BLECallbacks::scanResult(NimBLEAdvertisedDevice* advertisedDevice) {
    // Check if this is a BitChat device
    if (advertisedDevice->isAdvertisingService(NimBLEUUID(BITCHAT_SERVICE_UUID))) {
        String deviceName = advertisedDevice->getName().c_str();
        String address = advertisedDevice->getAddress().toString().c_str();
        
        Serial.printf("BLE Mesh: Found BitChat device: %s (%s) RSSI: %d\n", 
                     deviceName.c_str(), address.c_str(), advertisedDevice->getRSSI());
        
        // Don't connect to ourselves
        if (deviceName == BLEMesh::getPeerID()) {
            return;
        }
        
        // Attempt to connect
        BLEMesh::connectToPeer(advertisedDevice);
    }
}

void BLECallbacks::clientConnect(NimBLEClient* pClient) {
    Serial.printf("BLE Mesh: Connected to server %s\n", pClient->getPeerAddress().toString().c_str());
    pClient->updateConnParams(120, 120, 0, 60); // Update connection parameters for better performance
}

void BLECallbacks::clientDisconnect(NimBLEClient* pClient) {
    Serial.printf("BLE Mesh: Disconnected from server %s\n", pClient->getPeerAddress().toString().c_str());
}

void BLECallbacks::characteristicNotify(NimBLERemoteCharacteristic* pBLERemoteCharacteristic, 
                                      uint8_t* pData, size_t length, bool isNotify) {
    // Find the peer ID for this characteristic
    String sourcePeerID = "unknown";
    NimBLEClient* pClient = pBLERemoteCharacteristic->getRemoteService()->getClient();
    String address = pClient->getPeerAddress().toString().c_str();
    
    // For now, just use the address as identifier
    sourcePeerID = address;
    
    BLEMesh::handleReceivedData(pData, length, sourcePeerID);
}