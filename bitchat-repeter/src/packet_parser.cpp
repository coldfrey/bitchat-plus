#include "bitchat_protocol.h"
#include <Arduino.h>

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