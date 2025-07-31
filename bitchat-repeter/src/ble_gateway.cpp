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
    Serial.println("[BLE] Initializing BLE server for iOS devices...");
    
    // Generate unique gateway ID from MAC address
    generateGatewayID();
    Serial.printf("[BLE] Generated Gateway ID: %s\n", gatewayID.c_str());
    
    // Initialize NimBLE with descriptive name
    String deviceName = "BitChat-" + gatewayID;
    Serial.printf("[BLE] Initializing NimBLE with device name: %s\n", deviceName.c_str());
    NimBLEDevice::init(deviceName.c_str());
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Maximum power for better range
    Serial.println("[BLE] Set transmission power to maximum (ESP_PWR_LVL_P9)");
    
    // Create BLE Server
    Serial.println("[BLE] Creating BLE server...");
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());
    Serial.println("[BLE] BLE server created with callbacks");
    
    // Create BitChat service
    Serial.printf("[BLE] Creating BitChat service with UUID: %s\n", BITCHAT_SERVICE_UUID);
    pService = pServer->createService(BITCHAT_SERVICE_UUID);
    
    // Create characteristic with read, write, and notify properties
    Serial.printf("[BLE] Creating BitChat characteristic with UUID: %s\n", BITCHAT_CHAR_UUID);
    pCharacteristic = pService->createCharacteristic(
        BITCHAT_CHAR_UUID,
        NIMBLE_PROPERTY::READ | 
        NIMBLE_PROPERTY::WRITE | 
        NIMBLE_PROPERTY::WRITE_NR | 
        NIMBLE_PROPERTY::NOTIFY
    );
    
    pCharacteristic->setCallbacks(new CharacteristicCallbacks());
    Serial.println("[BLE] Characteristic created with R/W/N properties and callbacks");
    
    // Start the service
    pService->start();
    Serial.println("[BLE] BitChat service started");
    
    // Start advertising to iOS devices
    Serial.println("[BLE] Starting advertising...");
    startAdvertising();
    
    // Initialize connection manager
    Serial.println("[BLE] Initializing connection manager...");
    ConnectionManager::init();
    
    Serial.println("[BLE] *** BLE Gateway Initialization Complete ***");
    Serial.printf("[BLE] Gateway ID: %s\n", gatewayID.c_str());
    Serial.printf("[BLE] Service UUID: %s\n", BITCHAT_SERVICE_UUID);
    Serial.printf("[BLE] Characteristic UUID: %s\n", BITCHAT_CHAR_UUID);
    Serial.printf("[BLE] Device Name: BitChat-%s\n", gatewayID.c_str());
    Serial.println("[BLE] Ready to accept iOS device connections!");
}

void BLEGateway::process() {
    // Process connection management
    ConnectionManager::process();
    
    // NimBLE handles most processing automatically
    static unsigned long lastUpdate = 0;
    static unsigned long lastAdvertisingCheck = 0;
    unsigned long now = millis();
    
    // Check advertising status more frequently
    if (now - lastAdvertisingCheck > 5000) { // Every 5 seconds
        lastAdvertisingCheck = now;
        
        if (!pServer->getConnectedCount() && !pAdvertising->isAdvertising()) {
            Serial.println("[BLE-ADV] No devices connected and advertising stopped - restarting...");
            startAdvertising();
        } else if (!pAdvertising->isAdvertising()) {
            Serial.println("[BLE-ADV] Advertising stopped unexpectedly - restarting...");
            startAdvertising();
        }
    }
    
    if (now - lastUpdate > 15000) { // Every 15 seconds
        lastUpdate = now;
        
        int connectedCount = pServer->getConnectedCount();
        int healthyCount = ConnectionManager::getHealthyConnectionCount();
        
        Serial.println("[BLE-STATUS] === BLE Gateway Status ===");
        Serial.printf("[BLE-STATUS] Connected devices: %d\n", connectedCount);
        Serial.printf("[BLE-STATUS] Healthy connections: %d\n", healthyCount);
        Serial.printf("[BLE-STATUS] Advertising active: %s\n", pAdvertising->isAdvertising() ? "Yes" : "No");
        Serial.printf("[BLE-STATUS] Server running: %s\n", pServer ? "Yes" : "No");
        
        // Update connection RSSI (simplified simulation for now)
        if (connectedCount > 0) {
            int simulatedRSSI = -60 + (random(-20, 20)); // Simulate -40 to -80 dBm range
            ConnectionManager::updateConnectionRSSI(1, simulatedRSSI);
            Serial.printf("[BLE-STATUS] Simulated RSSI update: %d dBm\n", simulatedRSSI);
        }
        
        // Print detailed connection statistics
        if (connectedCount > 0) {
            Serial.println("[BLE-STATUS] Connection details:");
            ConnectionManager::printConnectionStats();
        } else {
            Serial.println("[BLE-STATUS] No active connections");
        }
        
        Serial.println("[BLE-STATUS] === End BLE Status ===\n");
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
    Serial.println("[BLE-ADV] Configuring advertising parameters...");
    pAdvertising = NimBLEDevice::getAdvertising();
    
    // Add service UUID to advertisement
    pAdvertising->addServiceUUID(BITCHAT_SERVICE_UUID);
    Serial.printf("[BLE-ADV] Added service UUID: %s\n", BITCHAT_SERVICE_UUID);
    
    // Set a descriptive device name (appears in iOS scan results)
    String advertisingName = "BitChat-" + gatewayID;
    pAdvertising->setName(advertisingName.c_str());
    Serial.printf("[BLE-ADV] Set advertising name: %s\n", advertisingName.c_str());
    
    // Configure advertising parameters for better discoverability
    pAdvertising->setScanResponse(true);  // Enable scan response for more data
    pAdvertising->setMinPreferred(0x0);
    
    // Set advertising intervals (in 0.625ms units)
    pAdvertising->setMinInterval(160);  // 100ms
    pAdvertising->setMaxInterval(240);  // 150ms
    Serial.println("[BLE-ADV] Set advertising interval: 100-150ms");
    
    // Add manufacturer data with gateway info
    String mfgDataStr = "BC" + gatewayID.substring(0, 6);  // "BC" + first 6 chars of gateway ID
    std::string mfgData = mfgDataStr.c_str();
    pAdvertising->setManufacturerData(mfgData);
    Serial.printf("[BLE-ADV] Added manufacturer data: %s\n", mfgData.c_str());
    
    // Start advertising
    pAdvertising->start();
    
    Serial.println("[BLE-ADV] *** ADVERTISING STARTED ***");
    Serial.printf("[BLE-ADV] Device Name: %s\n", advertisingName.c_str());
    Serial.printf("[BLE-ADV] Service UUID: %s\n", BITCHAT_SERVICE_UUID);
    Serial.println("[BLE-ADV] Device is now discoverable by iOS apps!");
}

void BLEGateway::handleReceivedData(const uint8_t* data, size_t length, uint16_t connectionHandle) {
    Serial.println("\n[BLE-RX] *** MESSAGE RECEIVED ***");
    Serial.printf("[BLE-RX] Connection Handle: %d\n", connectionHandle);
    Serial.printf("[BLE-RX] Data Length: %d bytes\n", length);
    Serial.printf("[BLE-RX] Timestamp: %lu ms\n", millis());
    
    // Print complete hex dump for debugging
    Serial.print("[BLE-RX] Raw Data: ");
    for (size_t i = 0; i < length; i++) {
        Serial.printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) Serial.print("\n[BLE-RX]            ");
    }
    Serial.println();
    
    // Print as ASCII if printable
    Serial.print("[BLE-RX] ASCII: ");
    for (size_t i = 0; i < length; i++) {
        if (data[i] >= 32 && data[i] <= 126) {
            Serial.printf("%c", data[i]);
        } else {
            Serial.print(".");
        }
    }
    Serial.println();
    
    // Handle version negotiation first
    if (length >= 2 && data[1] == MSG_TYPE_VERSION_HELLO) {
        Serial.println("[BLE-RX] Detected VERSION_HELLO message");
        handleVersionHello(data, length, connectionHandle);
        Serial.println("[BLE-RX] *** END MESSAGE PROCESSING ***\n");
        return;
    }
    
    // Check if connection completed version negotiation
    if (!isConnectionReady(connectionHandle)) {
        Serial.printf("[BLE-RX] ❌ REJECTING MESSAGE - Version negotiation not completed for connection %d\n", connectionHandle);
        Serial.println("[BLE-RX] *** END MESSAGE PROCESSING ***\n");
        return;
    }
    
    Serial.println("[BLE-RX] ✅ Connection ready, parsing BitChat packet...");
    
    // Parse the BitChat packet
    BitchatPacket packet;
    ParseResult result = parsePacket(data, length, packet);
    
    if (result != PARSE_SUCCESS) {
        Serial.printf("[BLE-RX] ❌ PACKET PARSING FAILED - Connection %d, Error Code: %d\n", connectionHandle, result);
        Serial.println("[BLE-RX] Possible causes: Malformed packet, incorrect format, or corruption");
        Serial.println("[BLE-RX] *** END MESSAGE PROCESSING ***\n");
        return;
    }
    
    Serial.println("[BLE-RX] ✅ Packet parsed successfully!");
    
    // Convert sender ID to hex string for logging
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    
    // Convert recipient ID to hex string
    char recipientHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(recipientHex + (i * 2), "%02X", packet.recipientID[i]);
    }
    recipientHex[16] = '\0';
    
    // Determine message type string
    const char* msgTypeStr = "UNKNOWN";
    switch (packet.type) {
        case MSG_TYPE_ANNOUNCE: msgTypeStr = "ANNOUNCE"; break;
        case MSG_TYPE_LEAVE: msgTypeStr = "LEAVE"; break;
        case MSG_TYPE_MESSAGE: msgTypeStr = "MESSAGE"; break;
        case MSG_TYPE_DELIVERY_ACK: msgTypeStr = "DELIVERY_ACK"; break;
        case MSG_TYPE_PROTOCOL_ACK: msgTypeStr = "PROTOCOL_ACK"; break;
        case MSG_TYPE_VERSION_HELLO: msgTypeStr = "VERSION_HELLO"; break;
        case MSG_TYPE_VERSION_ACK: msgTypeStr = "VERSION_ACK"; break;
    }
    
    Serial.println("[BLE-RX] === PACKET DETAILS ===");
    Serial.printf("[BLE-RX] Type: %s (0x%02X)\n", msgTypeStr, packet.type);
    Serial.printf("[BLE-RX] From: %s\n", senderHex);
    Serial.printf("[BLE-RX] To: %s\n", recipientHex);
    Serial.printf("[BLE-RX] TTL: %d\n", packet.ttl);
    Serial.printf("[BLE-RX] Timestamp: %lu\n", packet.timestamp);
    Serial.printf("[BLE-RX] Payload Length: %d bytes\n", packet.payloadLength);
    
    // Print payload preview if available
    if (packet.payload && packet.payloadLength > 0) {
        Serial.print("[BLE-RX] Payload Preview: ");
        size_t previewLen = (packet.payloadLength < 32) ? packet.payloadLength : 32;
        for (size_t i = 0; i < previewLen; i++) {
            if (packet.payload[i] >= 32 && packet.payload[i] <= 126) {
                Serial.printf("%c", packet.payload[i]);
            } else {
                Serial.print(".");
            }
        }
        if (packet.payloadLength > 32) Serial.print("...");
        Serial.println();
    }
    
    Serial.println("[BLE-RX] Forwarding to message router for LoRa mesh relay...");
    
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
            Serial.printf("[BLE-RX] Unknown message type 0x%02X forwarded to LoRa mesh\n", packet.type);
            break;
    }
    
    Serial.println("[BLE-RX] *** END MESSAGE PROCESSING ***\n");
}

// Server callback implementations
void ServerCallbacks::onConnect(NimBLEServer* pServer) {
    int totalConnected = pServer->getConnectedCount();
    
    Serial.println("[BLE-CONN] === NEW DEVICE CONNECTED ===");
    Serial.printf("[BLE-CONN] Total connected devices: %d\n", totalConnected);
    
    // Simple connection handle mapping for now
    uint16_t connectionHandle = totalConnected;
    
    // Generate temporary device ID until VERSION_HELLO provides real one
    String tempDeviceID = "iOS-Device-" + String(connectionHandle);
    
    // Add to connection manager
    ConnectionManager::addConnection(connectionHandle, tempDeviceID);
    Serial.printf("[BLE-CONN] Added to connection manager with ID: %s\n", tempDeviceID.c_str());
    
    // Initialize connection state
    iOSConnectionState state;
    state.deviceID = tempDeviceID;
    state.connectTime = millis();
    BLEGateway::setConnectionState(connectionHandle, state);
    
    Serial.printf("[BLE-CONN] Connection handle: %d\n", connectionHandle);
    Serial.printf("[BLE-CONN] Connection time: %lu ms\n", millis());
    Serial.println("[BLE-CONN] Waiting for VERSION_HELLO from device...");
    
    // Keep advertising for multiple device connections
    Serial.println("[BLE-CONN] Keeping advertising active for additional connections");
    Serial.println("[BLE-CONN] === Connection Complete ===");
}

void ServerCallbacks::onDisconnect(NimBLEServer* pServer) {
    int remainingConnected = pServer->getConnectedCount();
    
    Serial.println("[BLE-DISC] === DEVICE DISCONNECTED ===");
    Serial.printf("[BLE-DISC] Remaining connected devices: %d\n", remainingConnected);
    
    // ConnectionManager will handle stale connection cleanup automatically
    Serial.println("[BLE-DISC] Connection manager will clean up stale connections");
    
    // Restart advertising if no clients connected
    if (remainingConnected == 0) {
        Serial.println("[BLE-DISC] No devices remaining - restarting advertising...");
        delay(500); // Brief delay
        pServer->startAdvertising();
        Serial.println("[BLE-DISC] Advertising restarted - ready for new connections");
    } else {
        Serial.printf("[BLE-DISC] %d device(s) still connected\n", remainingConnected);
    }
    
    Serial.println("[BLE-DISC] === Disconnect Complete ===");
}

// Characteristic callback implementations
void CharacteristicCallbacks::onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    
    if (value.length() > 0) {
        Serial.printf("[BLE-RX] Received %d bytes from iOS device\n", value.length());
        
        // Simplified connection handle mapping
        uint16_t connectionHandle = 1; // TODO: Improve connection handle tracking
        
        // Update connection activity
        ConnectionManager::updateConnectionActivity(connectionHandle, true);
        Serial.printf("[BLE-RX] Updated connection activity for handle %d\n", connectionHandle);
        
        BLEGateway::handleReceivedData((const uint8_t*)value.data(), value.length(), connectionHandle);
    } else {
        Serial.println("[BLE-RX] Received empty data - ignoring");
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