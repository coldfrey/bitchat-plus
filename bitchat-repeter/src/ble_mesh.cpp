#include "ble_mesh.h"
#include <Arduino.h>
#include <WiFi.h>

// Static member definitions
NimBLEServer* BLEMesh::pServer = nullptr;
NimBLEService* BLEMesh::pService = nullptr;
NimBLECharacteristic* BLEMesh::pCharacteristic = nullptr;
NimBLEAdvertising* BLEMesh::pAdvertising = nullptr;
String BLEMesh::myPeerID = "";
std::map<uint16_t, ConnectionState> BLEMesh::connectionStates;

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


void BLEMesh::handleReceivedData(const uint8_t* data, size_t length, uint16_t connectionHandle) {
    Serial.printf("BLE Mesh: Received %d bytes from connection %d\n", length, connectionHandle);
    
    // Print first few bytes for debugging
    Serial.print("BLE Mesh: Data: ");
    for (size_t i = 0; i < min(length, (size_t)16); i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
    
    // Quick check for VERSION_HELLO before full parsing
    if (length >= 2 && data[1] == MSG_TYPE_VERSION_HELLO) {
        Serial.println("BLE Mesh: Received VERSION_HELLO");
        handleVersionHello(data, length, connectionHandle);
        return;
    }
    
    // Check if connection completed version negotiation
    if (!isConnectionReady(connectionHandle)) {
        Serial.printf("BLE Mesh: Rejecting message from connection %d - version negotiation not completed\n", connectionHandle);
        return;
    }
    
    // Parse the BitChat packet
    BitchatPacket packet;
    ParseResult result = parsePacket(data, length, packet);
    
    if (result != PARSE_SUCCESS) {
        Serial.printf("BLE Mesh: Failed to parse packet from connection %d, error=%d\n", connectionHandle, result);
        return;
    }
    
    // Convert sender ID to hex string for logging
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    
    Serial.printf("BLE Mesh: Parsed packet - Type=0x%02X, From=%s, TTL=%d\n", 
                 packet.type, senderHex, packet.ttl);
    
    // Handle specific message types
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
            Serial.printf("BLE Mesh: Unhandled message type 0x%02X\n", packet.type);
            break;
    }
}

// Server callback implementations
void ServerCallbacks::onConnect(NimBLEServer* pServer) {
    Serial.printf("BLE Mesh: iOS device connected (total: %d)\n", pServer->getConnectedCount());
    
    // Get the connection handle and initialize connection state
    // Note: NimBLE doesn't easily expose connection handles in onConnect
    // We'll track this in the characteristic callback instead
    
    // Don't stop advertising - allow multiple iOS devices to connect
}

void ServerCallbacks::onDisconnect(NimBLEServer* pServer) {
    Serial.printf("BLE Mesh: iOS device disconnected (remaining: %d)\n", pServer->getConnectedCount());
    
    // Clean up connection states for disconnected devices
    // Note: We'll need to implement this when we can properly track connection handles
    
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
        // For now, use a simple connection handle mapping since NimBLE API is limited
        // In a full implementation, we'd track connection handles properly
        // For simplicity, we'll use a static counter or connection index
        uint16_t connectionHandle = 1; // Simplified for now - single connection
        
        BLEMesh::handleReceivedData((const uint8_t*)value.data(), value.length(), connectionHandle);
    }
}

// Version negotiation implementation
void BLEMesh::handleVersionHello(const uint8_t* data, size_t length, uint16_t connectionHandle) {
    Serial.printf("BLE Mesh: Processing VERSION_HELLO from connection %d\n", connectionHandle);
    
    // Minimum size check: version(1) + type(1) + senderID(8) + recipientID(8) + timestamp(8) + payloadLength(2) = 28 bytes minimum
    if (length < 28) {
        Serial.printf("BLE Mesh: VERSION_HELLO packet too small (%d bytes), ignoring\n", length);
        return;
    }
    
    // Parse the BitChat packet header
    uint8_t version = data[0];
    uint8_t type = data[1];
    
    // Extract sender ID (8 bytes starting at offset 2)
    char senderID[17]; // 8 bytes = 16 hex chars + null terminator
    for (int i = 0; i < 8; i++) {
        sprintf(senderID + (i * 2), "%02X", data[2 + i]);
    }
    senderID[16] = '\0';
    
    Serial.printf("BLE Mesh: VERSION_HELLO from peer %s, protocol version %d\n", senderID, version);
    
    // Extract payload length (2 bytes at offset 26)
    uint16_t payloadLength = (data[26] << 8) | data[27];
    
    // Verify we have enough data for the payload
    if (length < 28 + payloadLength) {
        Serial.printf("BLE Mesh: Incomplete VERSION_HELLO payload, expected %d bytes\n", 28 + payloadLength);
        return;
    }
    
    // The payload should contain the VersionHello JSON
    const uint8_t* payload = data + 28;
    
    // For simplicity, we'll just parse the version from the first byte of payload
    // In a full implementation, we'd parse the JSON to get supported versions
    uint8_t requestedVersion = (payloadLength > 0) ? payload[0] : version;
    
    // Determine agreed version (we only support version 1)
    uint8_t agreedVersion = 1;
    bool compatible = (requestedVersion == 1 || version == 1);
    
    if (!compatible) {
        Serial.printf("BLE Mesh: Incompatible version requested (%d), we only support version 1\n", requestedVersion);
        // In a full implementation, we'd send a rejection
        return;
    }
    
    // Update connection state
    ConnectionState& state = connectionStates[connectionHandle];
    state.peerID = String(senderID);
    state.negotiatedVersion = agreedVersion;
    state.isReady = true;
    state.connectTime = millis();
    
    Serial.printf("BLE Mesh: Version negotiation completed with peer %s, using version %d\n", 
                 senderID, agreedVersion);
    
    // Send VERSION_ACK response
    sendVersionAck(agreedVersion, connectionHandle);
}

void BLEMesh::sendVersionAck(uint8_t agreedVersion, uint16_t connectionHandle) {
    Serial.printf("BLE Mesh: Sending VERSION_ACK (version %d) to connection %d\n", agreedVersion, connectionHandle);
    
    // Get peer ID from connection state
    auto it = connectionStates.find(connectionHandle);
    if (it == connectionStates.end()) {
        Serial.println("BLE Mesh: Cannot send VERSION_ACK - connection state not found");
        return;
    }
    
    String peerID = it->second.peerID;
    
    // Create VERSION_ACK packet
    // Simplified version - in full implementation we'd create proper JSON payload
    uint8_t response[64];
    int responseLength = 0;
    
    // BitChat packet header
    response[responseLength++] = PROTOCOL_VERSION;  // version
    response[responseLength++] = MSG_TYPE_VERSION_ACK;  // type
    
    // Sender ID (our peer ID - 8 bytes)
    String myID = getPeerID();
    for (int i = 0; i < 8 && i < myID.length(); i += 2) {
        String hexByte = myID.substring(i, i + 2);
        response[responseLength++] = (uint8_t)strtol(hexByte.c_str(), nullptr, 16);
    }
    
    // Recipient ID (peer's ID - 8 bytes)
    for (int i = 0; i < 8 && i < peerID.length(); i += 2) {
        String hexByte = peerID.substring(i, i + 2);
        response[responseLength++] = (uint8_t)strtol(hexByte.c_str(), nullptr, 16);
    }
    
    // Timestamp (8 bytes) - current time in milliseconds
    uint64_t timestamp = millis();
    for (int i = 7; i >= 0; i--) {
        response[responseLength++] = (timestamp >> (i * 8)) & 0xFF;
    }
    
    // Payload length (2 bytes) - simplified payload
    uint16_t payloadLen = 1;
    response[responseLength++] = (payloadLen >> 8) & 0xFF;
    response[responseLength++] = payloadLen & 0xFF;
    
    // Simplified payload - just the agreed version
    response[responseLength++] = agreedVersion;
    
    // Send the response
    if (pCharacteristic && pServer->getConnectedCount() > 0) {
        pCharacteristic->setValue(response, responseLength);
        pCharacteristic->notify();
        Serial.printf("BLE Mesh: Sent VERSION_ACK (%d bytes) to peer %s\n", responseLength, peerID.c_str());
    } else {
        Serial.println("BLE Mesh: Cannot send VERSION_ACK - no connected devices");
    }
}

bool BLEMesh::isConnectionReady(uint16_t connectionHandle) {
    auto it = connectionStates.find(connectionHandle);
    if (it == connectionStates.end()) {
        return false;
    }
    return it->second.isReady;
}

// Message type handlers
void BLEMesh::handleAnnounceMessage(const BitchatPacket& packet, uint16_t connectionHandle) {
    // Extract nickname from payload
    String nickname = "";
    if (packet.payload && packet.payloadLength > 0) {
        // Convert payload to string (it should be UTF-8 text)
        char* buffer = (char*)malloc(packet.payloadLength + 1);
        memcpy(buffer, packet.payload, packet.payloadLength);
        buffer[packet.payloadLength] = '\0';
        nickname = String(buffer);
        free(buffer);
        nickname.trim(); // Remove whitespace
    }
    
    // Convert sender ID to hex string
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    
    Serial.printf("BLE Mesh: ANNOUNCE from %s: \"%s\" (TTL=%d)\n", 
                 senderHex, nickname.c_str(), packet.ttl);
    
    // TODO: Forward to message router for LoRa mesh relay
    // TODO: Update peer presence tracking
}

void BLEMesh::handleLeaveMessage(const BitchatPacket& packet, uint16_t connectionHandle) {
    // Convert sender ID to hex string
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    
    Serial.printf("BLE Mesh: LEAVE from %s (TTL=%d)\n", senderHex, packet.ttl);
    
    // TODO: Forward to message router for LoRa mesh relay
    // TODO: Update peer presence tracking (remove peer)
}

void BLEMesh::handleChatMessage(const BitchatPacket& packet, uint16_t connectionHandle) {
    // Convert sender and recipient IDs to hex strings
    char senderHex[17], recipientHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
        sprintf(recipientHex + (i * 2), "%02X", packet.recipientID[i]);
    }
    senderHex[16] = recipientHex[16] = '\0';
    
    // Check if this is a broadcast (all zeros recipient) or private message
    bool isBroadcast = true;
    for (int i = 0; i < 8; i++) {
        if (packet.recipientID[i] != 0) {
            isBroadcast = false;
            break;
        }
    }
    
    // Extract message text from payload (simplified - assumes plain text)
    String messageText = "";
    if (packet.payload && packet.payloadLength > 0) {
        char* buffer = (char*)malloc(packet.payloadLength + 1);
        memcpy(buffer, packet.payload, packet.payloadLength);
        buffer[packet.payloadLength] = '\0';
        messageText = String(buffer);
        free(buffer);
    }
    
    if (isBroadcast) {
        Serial.printf("BLE Mesh: BROADCAST MESSAGE from %s: \"%s\" (TTL=%d)\n", 
                     senderHex, messageText.c_str(), packet.ttl);
    } else {
        Serial.printf("BLE Mesh: PRIVATE MESSAGE from %s to %s: \"%s\" (TTL=%d)\n", 
                     senderHex, recipientHex, messageText.c_str(), packet.ttl);
    }
    
    // TODO: Forward to message router for LoRa mesh relay
    // TODO: Apply message filtering and routing logic
}

void BLEMesh::handleAckMessage(const BitchatPacket& packet, uint16_t connectionHandle) {
    // Convert sender ID to hex string
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    
    const char* ackType = (packet.type == MSG_TYPE_DELIVERY_ACK) ? "DELIVERY_ACK" : "PROTOCOL_ACK";
    
    Serial.printf("BLE Mesh: %s from %s (TTL=%d)\n", ackType, senderHex, packet.ttl);
    
    // TODO: Forward to message router for LoRa mesh relay
    // TODO: Update delivery tracking
}