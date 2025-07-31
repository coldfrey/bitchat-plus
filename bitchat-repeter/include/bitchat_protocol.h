#pragma once

#include <stdint.h>

// BitChat Protocol Constants (from iOS app)
// Service and Characteristic UUIDs
#define BITCHAT_SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define BITCHAT_RX_CHAR_UUID        "12345678-1234-5678-1234-56789abcdef1"
#define BITCHAT_TX_CHAR_UUID        "12345678-1234-5678-1234-56789abcdef2"

// Version negotiation constants
#define HELLO_MESSAGE               0xAA
#define ACK_MESSAGE                 0xAB
#define PROTOCOL_VERSION            3

// Message types
#define MSG_TYPE_BROADCAST          0x01
#define MSG_TYPE_PRIVATE            0x02
#define MSG_TYPE_PRESENCE           0x03
#define MSG_TYPE_FILE_TRANSFER      0x04
#define MSG_TYPE_TYPING_INDICATOR   0x05
#define MSG_TYPE_DELIVERY_ACK       0x06

// Packet structure (to be extracted from iOS app)
struct BitchatPacket {
    uint8_t messageType;
    uint32_t messageId;
    uint32_t timestamp;
    uint8_t ttl;
    uint16_t payloadLength;
    uint8_t* payload;
    
    // Constructor
    BitchatPacket() : messageType(0), messageId(0), timestamp(0), ttl(10), payloadLength(0), payload(nullptr) {}
};