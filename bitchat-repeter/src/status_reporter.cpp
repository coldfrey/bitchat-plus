#include "status_reporter.h"
#include "ble_mesh.h"
#include "lora_bridge.h"
#include "message_router.h"
#include "config_manager.h"
#include <Arduino.h>

// Static member definitions
unsigned long StatusReporter::lastStatusUpdate = 0;
uint32_t StatusReporter::totalMessagesSent = 0;
uint32_t StatusReporter::totalMessagesReceived = 0;
unsigned long StatusReporter::bootTime = 0;
const String StatusReporter::FIRMWARE_VERSION = "1.0.0";

void StatusReporter::init() {
    Serial.println("Status Reporter: Initializing...");
    bootTime = millis();
    lastStatusUpdate = 0; // Force immediate first status update
    
    Serial.printf("Status Reporter: Firmware version %s\n", FIRMWARE_VERSION.c_str());
    Serial.println("Status Reporter: Initialized successfully");
}

void StatusReporter::process() {
    unsigned long now = millis();
    
    // Send status update every 30 seconds
    if (now - lastStatusUpdate >= STATUS_UPDATE_INTERVAL_MS) {
        sendStatusUpdate();
        lastStatusUpdate = now;
    }
}

void StatusReporter::sendStatusUpdate() {
    Serial.println("Status Reporter: Sending status update...");
    
    // Create status presence packet
    BitchatPacket statusPacket = createStatusPacket();
    
    // Send via message router to all connected iOS devices
    MessageRouter::forwardToBLE(statusPacket);
    
    // Also increment our message sent counter
    totalMessagesSent++;
    
    Serial.printf("Status Reporter: Status update sent (uptime: %lu seconds, "
                 "BLE clients: %d, mesh neighbors: %d)\n",
                 getUptimeSeconds(), 
                 BLEMesh::getConnectedClientCount(),
                 NeighborTable::getNeighborCount());
}

String StatusReporter::getRepeaterNickname() {
    // Format: [Repeater] DeviceName
    String deviceName = ConfigManager::getDeviceName();
    return "[Repeater] " + deviceName;
}

String StatusReporter::getFirmwareVersion() {
    return FIRMWARE_VERSION;
}

BitchatPacket StatusReporter::createStatusPacket() {
    BitchatPacket packet;
    
    // Set packet type to announce (presence)
    packet.type = MSG_TYPE_ANNOUNCE;
    
    // Set sender ID to our peer ID (convert from hex string to bytes)
    String peerID = BLEMesh::getPeerID();
    memset(packet.senderID, 0, 8);
    
    // Convert hex string peer ID to bytes
    for (int i = 0; i < min(8, (int)peerID.length() / 2); i++) {
        String byteStr = peerID.substring(i * 2, i * 2 + 2);
        packet.senderID[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
    }
    
    // Set as broadcast message
    memset(packet.recipientID, 0, 8);
    
    // Set TTL
    packet.ttl = 5;
    
    // Set timestamp
    packet.timestamp = millis();
    
    // Create status message payload
    String statusMessage = formatStatusMessage();
    
    // Allocate payload and copy status message
    packet.payloadLength = statusMessage.length();
    packet.payload = (uint8_t*)malloc(packet.payloadLength);
    memcpy(packet.payload, statusMessage.c_str(), packet.payloadLength);
    
    return packet;
}

String StatusReporter::formatStatusMessage() {
    // Get current status information
    int bleConnections = BLEMesh::getConnectedClientCount();
    size_t meshNeighbors = NeighborTable::getNeighborCount();
    int loraRSSI = LoRaBridge::getLastRSSI();
    String deviceName = ConfigManager::getDeviceName();
    unsigned long uptime = getUptimeSeconds();
    
    // Format comprehensive status message
    String status = getRepeaterNickname();
    status += " | ";
    status += "FW:" + FIRMWARE_VERSION;
    status += " | ";
    status += "BLE:" + String(bleConnections);
    status += " | ";
    status += "Mesh:" + String(meshNeighbors);
    
    if (loraRSSI != 0) {
        status += " | ";
        status += "RSSI:" + String(loraRSSI) + "dBm";
    }
    
    status += " | ";
    status += "Up:" + String(uptime / 3600) + "h" + String((uptime % 3600) / 60) + "m";
    
    // Add message statistics
    status += " | ";
    status += "TX:" + String(totalMessagesSent);
    status += "/RX:" + String(totalMessagesReceived);
    
    return status;
}

uint32_t StatusReporter::getTotalMessagesSent() {
    return totalMessagesSent;
}

uint32_t StatusReporter::getTotalMessagesReceived() {
    return totalMessagesReceived;
}

unsigned long StatusReporter::getUptimeSeconds() {
    return (millis() - bootTime) / 1000;
}