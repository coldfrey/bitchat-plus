#include "communication/ble_gateway.h"
#include "communication/message_router.h"
#include "core/connection_manager.h"
#include <Arduino.h>
#include <WiFi.h>

// Static member definitions
NimBLEServer* BLEGateway::pServer = nullptr;
NimBLEService* BLEGateway::pService = nullptr;
NimBLECharacteristic* BLEGateway::pCharacteristic = nullptr;
NimBLEAdvertising* BLEGateway::pAdvertising = nullptr;
String BLEGateway::gatewayID = "";
std::map<uint16_t, iOSConnectionState> BLEGateway::connectionStates;

void BLEGateway::init() {
    Serial.println("BLE Gateway: Initializing BLE server for iOS devices...");
    
    // Generate unique gateway ID from MAC address
    generateGatewayID();
    
    // Initialize NimBLE
    NimBLEDevice::init("BitChat-Gateway");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Maximum power for better range
    
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
    
    // Start advertising to iOS devices
    startAdvertising();
    
    // Initialize connection manager
    ConnectionManager::init();
    
    Serial.printf("BLE Gateway: Ready with ID: %s\n", gatewayID.c_str());
    Serial.printf("BLE Gateway: Advertising BitChat service: %s\n", BITCHAT_SERVICE_UUID);
}

void BLEGateway::process() {
    // Process connection management
    ConnectionManager::process();
    
    // NimBLE handles most processing automatically
    static unsigned long lastUpdate = 0;
    unsigned long now = millis();
    
    if (now - lastUpdate > 10000) { // Every 10 seconds
        lastUpdate = now;
        
        // Restart advertising if no devices connected
        if (!pServer->getConnectedCount() && !pAdvertising->isAdvertising()) {
            Serial.println("BLE Gateway: Restarting advertising");
            startAdvertising();
        }
        
        Serial.printf("BLE Gateway: Connected iOS devices: %d (healthy: %d)\n", 
                     pServer->getConnectedCount(), ConnectionManager::getHealthyConnectionCount());
        
        // Update connection RSSI (simplified simulation for now)
        if (pServer->getConnectedCount() > 0) {
            int simulatedRSSI = -60 + (random(-20, 20)); // Simulate -40 to -80 dBm range
            ConnectionManager::updateConnectionRSSI(1, simulatedRSSI);
        }
        
        // Print connection statistics
        if (pServer->getConnectedCount() > 0) {
            ConnectionManager::printConnectionStats();
        }
    }
}

String BLEGateway::getGatewayID() {
    return gatewayID;
}

void BLEGateway::sendToiOSDevices(const uint8_t* data, size_t length) {
    // Send to all connected iOS devices
    if (pCharacteristic && pServer->getConnectedCount() > 0) {
        pCharacteristic->setValue(data, length);
        pCharacteristic->notify();
        Serial.printf("BLE Gateway: Sent %d bytes to %d connected iOS device(s)\n", 
                     length, pServer->getConnectedCount());
    } else {
        Serial.printf("BLE Gateway: No iOS devices connected - message not sent (%d bytes)\n", length);
    }
}

int BLEGateway::getConnectedDeviceCount() {
    return pServer->getConnectedCount();
}

void BLEGateway::setConnectionState(uint16_t connectionHandle, const iOSConnectionState& state) {
    connectionStates[connectionHandle] = state;
}

void BLEGateway::setAdvertisingInterval(uint32_t intervalMs) {
    if (pAdvertising) {
        // NimBLE uses units of 0.625ms
        uint32_t intervalUnits = intervalMs / 0.625;
        
        // Clamp to valid range (20ms to 10.24s)
        if (intervalUnits < 32) intervalUnits = 32;       // 20ms minimum
        if (intervalUnits > 16384) intervalUnits = 16384; // 10.24s maximum
        
        // Stop advertising before changing interval
        bool wasAdvertising = pAdvertising->isAdvertising();
        if (wasAdvertising) {
            pAdvertising->stop();
        }
        
        // Set new interval
        pAdvertising->setMinInterval(intervalUnits);
        pAdvertising->setMaxInterval(intervalUnits);
        
        // Restart advertising if it was running
        if (wasAdvertising) {
            pAdvertising->start();
        }
        
        Serial.printf("BLE Gateway: Advertising interval set to %dms\n", intervalMs);
    } else {
        Serial.println("BLE Gateway: Cannot set advertising interval - not initialized");
    }
}

void BLEGateway::generateGatewayID() {
    // Get MAC address and create 8-character hex gateway ID
    uint8_t mac[6];
    WiFi.macAddress(mac);
    
    // Use first 4 bytes of MAC for gateway ID
    char idBuffer[9];
    snprintf(idBuffer, sizeof(idBuffer), "%02X%02X%02X%02X", 
             mac[0], mac[1], mac[2], mac[3]);
    
    gatewayID = String(idBuffer);
}

void BLEGateway::startAdvertising() {
    pAdvertising = NimBLEDevice::getAdvertising();
    
    // Add service UUID to advertisement
    pAdvertising->addServiceUUID(BITCHAT_SERVICE_UUID);
    
    // Set the gateway ID as the device name (appears in iOS scan results)
    pAdvertising->setName(gatewayID.c_str());
    
    // Configure advertising parameters
    pAdvertising->setScanResponse(false);
    pAdvertising->setMinPreferred(0x0);
    
    // Start advertising
    pAdvertising->start();
    
    Serial.println("BLE Gateway: Started advertising to iOS devices");
}

void BLEGateway::handleReceivedData(const uint8_t* data, size_t length, uint16_t connectionHandle) {
    Serial.printf("BLE Gateway: Received %d bytes from iOS device (connection %d)\n", length, connectionHandle);
    
    // Print first few bytes for debugging
    Serial.print("BLE Gateway: Data: ");
    for (size_t i = 0; i < min(length, (size_t)16); i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
    
    // Handle version negotiation first
    if (length >= 2 && data[1] == MSG_TYPE_VERSION_HELLO) {
        Serial.println("BLE Gateway: Received VERSION_HELLO from iOS device");
        handleVersionHello(data, length, connectionHandle);
        return;
    }
    
    // Check if connection completed version negotiation
    if (!isConnectionReady(connectionHandle)) {
        Serial.printf("BLE Gateway: Rejecting message from iOS device %d - version negotiation not completed\n", connectionHandle);
        return;
    }
    
    // Parse the BitChat packet
    BitchatPacket packet;
    ParseResult result = parsePacket(data, length, packet);
    
    if (result != PARSE_SUCCESS) {
        Serial.printf("BLE Gateway: Failed to parse packet from iOS device %d, error=%d\n", connectionHandle, result);
        return;
    }
    
    // Convert sender ID to hex string for logging
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    
    Serial.printf("BLE Gateway: Parsed packet from iOS - Type=0x%02X, From=%s, TTL=%d\n", 
                 packet.type, senderHex, packet.ttl);
    
    // Forward to message router for LoRa mesh relay
    MessageRouter::handleBLEMessage(packet, connectionHandle);
    
    // Handle specific message types for local logging
    switch (packet.type) {
        case MSG_TYPE_ANNOUNCE:
            handleAnnounceMessage(packet, connectionHandle);
            break;
            
        case MSG_TYPE_LEAVE:
            handleLeaveMessage(packet, connectionHandle);
            break;
            
        case MSG_TYPE_MESSAGE:
            handleChatMessage(packet, connectionHandle);
            break;
            
        case MSG_TYPE_DELIVERY_ACK:
        case MSG_TYPE_PROTOCOL_ACK:
            handleAckMessage(packet, connectionHandle);
            break;
            
        default:
            Serial.printf("BLE Gateway: Message type 0x%02X forwarded to LoRa mesh\n", packet.type);
            break;
    }
}

// Server callback implementations
void ServerCallbacks::onConnect(NimBLEServer* pServer) {
    Serial.printf("BLE Gateway: iOS device connected (total: %d)\n", pServer->getConnectedCount());
    
    // Simple connection handle mapping for now
    uint16_t connectionHandle = pServer->getConnectedCount();
    
    // Generate temporary device ID until VERSION_HELLO provides real one
    String tempDeviceID = "iOS-Device-" + String(connectionHandle);
    
    // Add to connection manager
    ConnectionManager::addConnection(connectionHandle, tempDeviceID);
    
    // Initialize connection state
    iOSConnectionState state;
    state.deviceID = tempDeviceID;
    state.connectTime = millis();
    BLEGateway::setConnectionState(connectionHandle, state);
    
    // Keep advertising for multiple device connections
}

void ServerCallbacks::onDisconnect(NimBLEServer* pServer) {
    Serial.printf("BLE Gateway: iOS device disconnected (remaining: %d)\n", pServer->getConnectedCount());
    
    // ConnectionManager will handle stale connection cleanup automatically
    
    // Restart advertising if no clients connected
    if (pServer->getConnectedCount() == 0) {
        delay(500); // Brief delay
        pServer->startAdvertising();
        Serial.println("BLE Gateway: Restarted advertising after disconnect");
    }
}

// Characteristic callback implementations
void CharacteristicCallbacks::onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    
    if (value.length() > 0) {
        // Simplified connection handle mapping
        uint16_t connectionHandle = 1; // TODO: Improve connection handle tracking
        
        // Update connection activity
        ConnectionManager::updateConnectionActivity(connectionHandle, true);
        
        BLEGateway::handleReceivedData((const uint8_t*)value.data(), value.length(), connectionHandle);
    }
}

// Version negotiation implementation
void BLEGateway::handleVersionHello(const uint8_t* data, size_t length, uint16_t connectionHandle) {
    Serial.printf("BLE Gateway: Processing VERSION_HELLO from iOS device %d\n", connectionHandle);
    
    // Minimum size check
    if (length < 28) {
        Serial.printf("BLE Gateway: VERSION_HELLO packet too small (%d bytes), ignoring\n", length);
        return;
    }
    
    // Parse packet header
    uint8_t version = data[0];
    
    // Extract sender ID (iOS device ID)
    char deviceID[17];
    for (int i = 0; i < 8; i++) {
        sprintf(deviceID + (i * 2), "%02X", data[2 + i]);
    }
    deviceID[16] = '\0';
    
    Serial.printf("BLE Gateway: VERSION_HELLO from iOS device %s, protocol version %d\n", deviceID, version);
    
    // We only support version 1
    uint8_t agreedVersion = 1;
    bool compatible = (version == 1);
    
    if (!compatible) {
        Serial.printf("BLE Gateway: Incompatible version %d from iOS device, rejecting\n", version);
        return;
    }
    
    // Update connection state
    iOSConnectionState& state = connectionStates[connectionHandle];
    state.deviceID = String(deviceID);
    state.negotiatedVersion = agreedVersion;
    state.isReady = true;
    state.connectTime = millis();
    
    Serial.printf("BLE Gateway: Version negotiation completed with iOS device %s, using version %d\n", 
                 deviceID, agreedVersion);
    
    // Send VERSION_ACK response
    sendVersionAck(agreedVersion, connectionHandle);
}

void BLEGateway::sendVersionAck(uint8_t agreedVersion, uint16_t connectionHandle) {
    Serial.printf("BLE Gateway: Sending VERSION_ACK (version %d) to iOS device %d\n", agreedVersion, connectionHandle);
    
    auto it = connectionStates.find(connectionHandle);
    if (it == connectionStates.end()) {
        Serial.println("BLE Gateway: Cannot send VERSION_ACK - connection state not found");
        return;
    }
    
    String deviceID = it->second.deviceID;
    
    // Create VERSION_ACK packet
    uint8_t response[64];
    int responseLength = 0;
    
    // BitChat packet header
    response[responseLength++] = PROTOCOL_VERSION;  // version
    response[responseLength++] = MSG_TYPE_VERSION_ACK;  // type
    
    // Sender ID (our gateway ID)
    String myID = getGatewayID();
    for (int i = 0; i < 8 && i < myID.length(); i += 2) {
        String hexByte = myID.substring(i, i + 2);
        response[responseLength++] = (uint8_t)strtol(hexByte.c_str(), nullptr, 16);
    }
    
    // Recipient ID (iOS device ID)
    for (int i = 0; i < 8 && i < deviceID.length(); i += 2) {
        String hexByte = deviceID.substring(i, i + 2);
        response[responseLength++] = (uint8_t)strtol(hexByte.c_str(), nullptr, 16);
    }
    
    // Timestamp (8 bytes)
    uint64_t timestamp = millis();
    for (int i = 7; i >= 0; i--) {
        response[responseLength++] = (timestamp >> (i * 8)) & 0xFF;
    }
    
    // Payload length (2 bytes) - just the version
    uint16_t payloadLen = 1;
    response[responseLength++] = (payloadLen >> 8) & 0xFF;
    response[responseLength++] = payloadLen & 0xFF;
    
    // Payload - agreed version
    response[responseLength++] = agreedVersion;
    
    // Send the response
    if (pCharacteristic && pServer->getConnectedCount() > 0) {
        pCharacteristic->setValue(response, responseLength);
        pCharacteristic->notify();
        Serial.printf("BLE Gateway: Sent VERSION_ACK (%d bytes) to iOS device %s\n", responseLength, deviceID.c_str());
    } else {
        Serial.println("BLE Gateway: Cannot send VERSION_ACK - no connected devices");
    }
}

bool BLEGateway::isConnectionReady(uint16_t connectionHandle) {
    auto it = connectionStates.find(connectionHandle);
    if (it == connectionStates.end()) {
        return false;
    }
    return it->second.isReady;
}

// Message type handlers (for logging/debugging)
void BLEGateway::handleAnnounceMessage(const BitchatPacket& packet, uint16_t connectionHandle) {
    // Extract nickname from payload
    String nickname = "";
    if (packet.payload && packet.payloadLength > 0) {
        char* buffer = (char*)malloc(packet.payloadLength + 1);
        if (buffer) {
            memcpy(buffer, packet.payload, packet.payloadLength);
            buffer[packet.payloadLength] = '\0';
            nickname = String(buffer);
            free(buffer);
            nickname.trim();
        }
    }
    
    // Convert sender ID to hex string
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    
    Serial.printf("BLE Gateway: iOS ANNOUNCE from %s: \"%s\" (TTL=%d) -> forwarding to LoRa mesh\n", 
                 senderHex, nickname.c_str(), packet.ttl);
}

void BLEGateway::handleLeaveMessage(const BitchatPacket& packet, uint16_t connectionHandle) {
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    
    Serial.printf("BLE Gateway: iOS LEAVE from %s (TTL=%d) -> forwarding to LoRa mesh\n", senderHex, packet.ttl);
}

void BLEGateway::handleChatMessage(const BitchatPacket& packet, uint16_t connectionHandle) {
    // Convert sender and recipient IDs to hex strings
    char senderHex[17], recipientHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
        sprintf(recipientHex + (i * 2), "%02X", packet.recipientID[i]);
    }
    senderHex[16] = recipientHex[16] = '\0';
    
    // Check if broadcast or private message
    bool isBroadcast = true;
    for (int i = 0; i < 8; i++) {
        if (packet.recipientID[i] != 0) {
            isBroadcast = false;
            break;
        }
    }
    
    // Extract message text (simplified)
    String messageText = "";
    if (packet.payload && packet.payloadLength > 0) {
        char* buffer = (char*)malloc(packet.payloadLength + 1);
        if (buffer) {
            memcpy(buffer, packet.payload, packet.payloadLength);
            buffer[packet.payloadLength] = '\0';
            messageText = String(buffer);
            free(buffer);
        }
    }
    
    if (isBroadcast) {
        Serial.printf("BLE Gateway: iOS BROADCAST from %s: \"%s\" (TTL=%d) -> forwarding to LoRa mesh\n", 
                     senderHex, messageText.c_str(), packet.ttl);
    } else {
        Serial.printf("BLE Gateway: iOS PRIVATE MESSAGE from %s to %s: \"%s\" (TTL=%d) -> forwarding to LoRa mesh\n", 
                     senderHex, recipientHex, messageText.c_str(), packet.ttl);
    }
}

void BLEGateway::handleAckMessage(const BitchatPacket& packet, uint16_t connectionHandle) {
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    
    const char* ackType = (packet.type == MSG_TYPE_DELIVERY_ACK) ? "DELIVERY_ACK" : "PROTOCOL_ACK";
    
    Serial.printf("BLE Gateway: iOS %s from %s (TTL=%d) -> forwarding to LoRa mesh\n", ackType, senderHex, packet.ttl);
}