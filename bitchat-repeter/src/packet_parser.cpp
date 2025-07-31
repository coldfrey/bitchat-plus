#include "core/bitchat_protocol.h"
#include <Arduino.h>
#include <esp_system.h>

// Check if a message type is supported in our simplified v1 implementation
bool isSupportedMessageType(uint8_t type) {
    switch (type) {
        // Essential message types for v1
        case MSG_TYPE_ANNOUNCE:           // 0x01 - Peer presence
        case MSG_TYPE_LEAVE:              // 0x03 - Peer leaving
        case MSG_TYPE_MESSAGE:            // 0x04 - User messages
        case MSG_TYPE_VERSION_HELLO:      // 0x20 - Already implemented
        case MSG_TYPE_VERSION_ACK:        // 0x21 - Already implemented
        case MSG_TYPE_DELIVERY_ACK:       // 0x0A - Message delivery ACK
        case MSG_TYPE_PROTOCOL_ACK:       // 0x22 - Protocol ACK
            return true;
            
        // Unsupported in v1 (too complex or not essential)
        case MSG_TYPE_FRAGMENT_START:     // 0x05 - File transfer (skip)
        case MSG_TYPE_FRAGMENT_CONTINUE:  // 0x06 - File transfer (skip)
        case MSG_TYPE_FRAGMENT_END:       // 0x07 - File transfer (skip)
        case MSG_TYPE_NOISE_HANDSHAKE_INIT:  // 0x10 - Complex encryption (skip)
        case MSG_TYPE_NOISE_HANDSHAKE_RESP:  // 0x11 - Complex encryption (skip)
        case MSG_TYPE_NOISE_ENCRYPTED:       // 0x12 - Complex encryption (skip)
        case MSG_TYPE_NOISE_IDENTITY:        // 0x13 - Complex encryption (skip)
        case MSG_TYPE_DELIVERY_STATUS:       // 0x0B - Not critical (skip)
        case MSG_TYPE_READ_RECEIPT:          // 0x0C - Not critical (skip)
        case MSG_TYPE_PROTOCOL_NACK:         // 0x23 - Error handling (skip for now)
        case MSG_TYPE_SYSTEM_VALIDATION:     // 0x24 - Advanced feature (skip)
        case MSG_TYPE_HANDSHAKE_REQUEST:     // 0x25 - Advanced feature (skip)
        case MSG_TYPE_FAVORITED:             // 0x30 - Advanced feature (skip)
        case MSG_TYPE_UNFAVORITED:           // 0x31 - Advanced feature (skip)
        default:
            return false;
    }
}

// Parse a BitChat packet from binary data
ParseResult parsePacket(const uint8_t* data, size_t length, BitchatPacket& packet) {
    // Validate data pointer to prevent null pointer dereference
    if (data == nullptr) {
        Serial.println("Packet Parser: Null data pointer provided");
        return PARSE_INCOMPLETE;
    }
    
    // Minimum packet size: version(1) + type(1) + senderID(8) + recipientID(8) + timestamp(8) + ttl(1) + payloadLength(2) = 29 bytes
    const size_t MIN_PACKET_SIZE = 29;
    
    if (length < MIN_PACKET_SIZE) {
        Serial.printf("Packet Parser: Packet too small (%d bytes, need at least %d)\n", length, MIN_PACKET_SIZE);
        return PARSE_TOO_SMALL;
    }
    
    size_t offset = 0;
    
    // Parse version (1 byte)
    packet.version = data[offset++];
    if (packet.version != PROTOCOL_VERSION) {
        Serial.printf("Packet Parser: Invalid version %d, expected %d\n", packet.version, PROTOCOL_VERSION);
        return PARSE_INVALID_VERSION;
    }
    
    // Parse type (1 byte)
    packet.type = data[offset++];
    if (!isSupportedMessageType(packet.type)) {
        Serial.printf("Packet Parser: Unsupported message type 0x%02X\n", packet.type);
        return PARSE_UNSUPPORTED_TYPE;
    }
    
    // Parse sender ID (8 bytes)
    memcpy(packet.senderID, data + offset, 8);
    offset += 8;
    
    // Parse recipient ID (8 bytes)
    memcpy(packet.recipientID, data + offset, 8);
    offset += 8;
    
    // Parse timestamp (8 bytes, big-endian)
    packet.timestamp = 0;
    for (int i = 0; i < 8; i++) {
        packet.timestamp = (packet.timestamp << 8) | data[offset++];
    }
    
    // Parse TTL (1 byte)
    packet.ttl = data[offset++];
    
    // Parse payload length (2 bytes, big-endian)
    packet.payloadLength = (data[offset] << 8) | data[offset + 1];
    offset += 2;
    
    // Verify we have enough data for the payload
    if (length < offset + packet.payloadLength) {
        Serial.printf("Packet Parser: Incomplete packet, expected %d bytes for payload\n", offset + packet.payloadLength);
        return PARSE_INCOMPLETE;
    }
    
    // Set payload pointer (points to data within the original buffer)
    packet.payload = (uint8_t*)(data + offset);
    
    // Debug output
    char senderHex[17], recipientHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
        sprintf(recipientHex + (i * 2), "%02X", packet.recipientID[i]);
    }
    senderHex[16] = recipientHex[16] = '\0';
    
    Serial.printf("Packet Parser: SUCCESS - Type=0x%02X, From=%s, To=%s, TTL=%d, PayloadLen=%d\n",
                 packet.type, senderHex, recipientHex, packet.ttl, packet.payloadLength);
    
    return PARSE_SUCCESS;
}

// Serialize a BitChat packet to binary data
size_t serializePacket(const BitchatPacket& packet, uint8_t* buffer, size_t bufferSize) {
    // Calculate required size
    const size_t PACKET_HEADER_SIZE = 29; // version + type + senderID + recipientID + timestamp + ttl + payloadLength
    size_t totalSize = PACKET_HEADER_SIZE + packet.payloadLength;
    
    if (bufferSize < totalSize) {
        Serial.printf("Packet Parser: Buffer too small for serialization (%d needed, %d available)\n", 
                     totalSize, bufferSize);
        return 0;
    }
    
    size_t offset = 0;
    
    // Serialize version (1 byte)
    buffer[offset++] = packet.version;
    
    // Serialize type (1 byte)
    buffer[offset++] = packet.type;
    
    // Serialize sender ID (8 bytes)
    memcpy(buffer + offset, packet.senderID, 8);
    offset += 8;
    
    // Serialize recipient ID (8 bytes)
    memcpy(buffer + offset, packet.recipientID, 8);
    offset += 8;
    
    // Serialize timestamp (8 bytes, big-endian)
    for (int i = 7; i >= 0; i--) {
        buffer[offset++] = (packet.timestamp >> (i * 8)) & 0xFF;
    }
    
    // Serialize TTL (1 byte)
    buffer[offset++] = packet.ttl;
    
    // Serialize payload length (2 bytes, big-endian)
    buffer[offset++] = (packet.payloadLength >> 8) & 0xFF;
    buffer[offset++] = packet.payloadLength & 0xFF;
    
    // Serialize payload
    if (packet.payload && packet.payloadLength > 0) {
        memcpy(buffer + offset, packet.payload, packet.payloadLength);
        offset += packet.payloadLength;
    }
    
    Serial.printf("Packet Parser: Serialized %d bytes (header=%d, payload=%d)\n", 
                 offset, PACKET_HEADER_SIZE, packet.payloadLength);
    
    return offset;
}

// Check if a LoRa packet type is supported
bool isSupportedLoRaPacketType(uint8_t type) {
    switch (type) {
        case LORA_PKT_NEIGHBOR_ANNOUNCE:  // 0x01 - Neighbor discovery
        case LORA_PKT_ROUTE_REQUEST:      // 0x02 - Route discovery request
        case LORA_PKT_ROUTE_REPLY:        // 0x03 - Route discovery reply
        case LORA_PKT_DATA:               // 0x04 - Data packet (contains BitChat packet)
        case LORA_PKT_MESH_ACK:           // 0x05 - Mesh acknowledgment
            return true;
        default:
            return false;
    }
}

// Parse a LoRa packet from binary data
LoRaParseResult parseLoRaPacket(const uint8_t* data, size_t length, LoRaPacket& packet) {
    // Minimum packet size: header (20 bytes) + at least 1 byte payload
    if (length < LORA_HEADER_SIZE) {
        Serial.printf("LoRa Parser: Packet too small (%d bytes, need at least %d)\n", length, LORA_HEADER_SIZE);
        return LORA_PARSE_TOO_SMALL;
    }
    
    size_t offset = 0;
    
    // Parse magic (1 byte)
    packet.magic = data[offset++];
    if (packet.magic != LORA_MAGIC) {
        Serial.printf("LoRa Parser: Invalid magic 0x%02X, expected 0x%02X\n", packet.magic, LORA_MAGIC);
        return LORA_PARSE_INVALID_MAGIC;
    }
    
    // Parse version (1 byte)
    packet.version = data[offset++];
    if (packet.version != LORA_VERSION) {
        Serial.printf("LoRa Parser: Invalid version 0x%02X, expected 0x%02X\n", packet.version, LORA_VERSION);
        return LORA_PARSE_INVALID_VERSION;
    }
    
    // Parse type (1 byte)
    packet.type = data[offset++];
    if (!isSupportedLoRaPacketType(packet.type)) {
        Serial.printf("LoRa Parser: Unsupported packet type 0x%02X\n", packet.type);
        return LORA_PARSE_UNSUPPORTED_TYPE;
    }
    
    // Parse source repeater ID (4 bytes, big-endian)
    packet.srcRepeater = 0;
    for (int i = 0; i < 4; i++) {
        packet.srcRepeater = (packet.srcRepeater << 8) | data[offset++];
    }
    
    // Parse destination repeater ID (4 bytes, big-endian)
    packet.destRepeater = 0;
    for (int i = 0; i < 4; i++) {
        packet.destRepeater = (packet.destRepeater << 8) | data[offset++];
    }
    
    // Parse next hop repeater ID (4 bytes, big-endian)
    packet.nextHop = 0;
    for (int i = 0; i < 4; i++) {
        packet.nextHop = (packet.nextHop << 8) | data[offset++];
    }
    
    // Parse hop count (1 byte)
    packet.hopCount = data[offset++];
    
    // Parse max hops (1 byte)
    packet.maxHops = data[offset++];
    if (packet.maxHops > LORA_MAX_HOPS_LIMIT) {
        Serial.printf("LoRa Parser: Max hops %d exceeds limit %d\n", packet.maxHops, LORA_MAX_HOPS_LIMIT);
        packet.maxHops = LORA_MAX_HOPS_LIMIT;
    }
    
    // Parse sequence number (2 bytes, big-endian)
    packet.seqNum = (data[offset] << 8) | data[offset + 1];
    offset += 2;
    
    // Parse payload length (1 byte)
    packet.payloadLen = data[offset++];
    if (packet.payloadLen > LORA_MAX_PAYLOAD_SIZE) {
        Serial.printf("LoRa Parser: Payload length %d exceeds maximum %d\n", packet.payloadLen, LORA_MAX_PAYLOAD_SIZE);
        return LORA_PARSE_PAYLOAD_TOO_LARGE;
    }
    
    // Verify we have enough data for the payload
    if (length < offset + packet.payloadLen) {
        Serial.printf("LoRa Parser: Incomplete packet, expected %d bytes for payload\n", offset + packet.payloadLen);
        return LORA_PARSE_TOO_SMALL;
    }
    
    // Copy payload data
    memcpy(packet.payload, data + offset, packet.payloadLen);
    
    // Debug output
    Serial.printf("LoRa Parser: SUCCESS - Type=0x%02X, Src=%08X, Dest=%08X, NextHop=%08X, Hops=%d/%d, Seq=%d, PayloadLen=%d\n",
                 packet.type, packet.srcRepeater, packet.destRepeater, packet.nextHop, 
                 packet.hopCount, packet.maxHops, packet.seqNum, packet.payloadLen);
    
    return LORA_PARSE_SUCCESS;
}

// Serialize a LoRa packet to binary data
size_t serializeLoRaPacket(const LoRaPacket& packet, uint8_t* buffer, size_t bufferSize) {
    // Calculate required size
    size_t totalSize = LORA_HEADER_SIZE + packet.payloadLen;
    
    if (bufferSize < totalSize) {
        Serial.printf("LoRa Parser: Buffer too small for serialization (%d needed, %d available)\n", 
                     totalSize, bufferSize);
        return 0;
    }
    
    if (packet.payloadLen > LORA_MAX_PAYLOAD_SIZE) {
        Serial.printf("LoRa Parser: Payload length %d exceeds maximum %d\n", packet.payloadLen, LORA_MAX_PAYLOAD_SIZE);
        return 0;
    }
    
    size_t offset = 0;
    
    // Serialize magic (1 byte)
    buffer[offset++] = packet.magic;
    
    // Serialize version (1 byte)
    buffer[offset++] = packet.version;
    
    // Serialize type (1 byte)
    buffer[offset++] = packet.type;
    
    // Serialize source repeater ID (4 bytes, big-endian)
    for (int i = 3; i >= 0; i--) {
        buffer[offset++] = (packet.srcRepeater >> (i * 8)) & 0xFF;
    }
    
    // Serialize destination repeater ID (4 bytes, big-endian)
    for (int i = 3; i >= 0; i--) {
        buffer[offset++] = (packet.destRepeater >> (i * 8)) & 0xFF;
    }
    
    // Serialize next hop repeater ID (4 bytes, big-endian)
    for (int i = 3; i >= 0; i--) {
        buffer[offset++] = (packet.nextHop >> (i * 8)) & 0xFF;
    }
    
    // Serialize hop count (1 byte)
    buffer[offset++] = packet.hopCount;
    
    // Serialize max hops (1 byte)
    buffer[offset++] = packet.maxHops;
    
    // Serialize sequence number (2 bytes, big-endian)
    buffer[offset++] = (packet.seqNum >> 8) & 0xFF;
    buffer[offset++] = packet.seqNum & 0xFF;
    
    // Serialize payload length (1 byte)
    buffer[offset++] = packet.payloadLen;
    
    // Serialize payload
    if (packet.payloadLen > 0) {
        memcpy(buffer + offset, packet.payload, packet.payloadLen);
        offset += packet.payloadLen;
    }
    
    Serial.printf("LoRa Parser: Serialized %d bytes (header=%d, payload=%d)\n", 
                 offset, LORA_HEADER_SIZE, packet.payloadLen);
    
    return offset;
}

// Generate repeater ID from MAC address
uint32_t getRepeaterID() {
    // Get MAC address from ESP32
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    // Create 32-bit ID from MAC address (use last 4 bytes)
    uint32_t id = 0;
    for (int i = 2; i < 6; i++) {  // Skip first 2 bytes to get unique part
        id = (id << 8) | mac[i];
    }
    
    Serial.printf("Repeater ID: %08X (from MAC: %02X:%02X:%02X:%02X:%02X:%02X)\n", 
                 id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return id;
}