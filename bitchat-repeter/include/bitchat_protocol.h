#pragma once

#include <stdint.h>

// BitChat Protocol Constants (extracted from iOS app)
// Service and Characteristic UUIDs (from BluetoothMeshService.swift)
#define BITCHAT_SERVICE_UUID        "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C"
#define BITCHAT_CHAR_UUID           "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D"

// Message Types (from BitchatProtocol.swift MessageType enum)
#define MSG_TYPE_ANNOUNCE           0x01
#define MSG_TYPE_LEAVE              0x03
#define MSG_TYPE_MESSAGE            0x04  // All user messages (private and broadcast)
#define MSG_TYPE_FRAGMENT_START     0x05
#define MSG_TYPE_FRAGMENT_CONTINUE  0x06  
#define MSG_TYPE_FRAGMENT_END       0x07
#define MSG_TYPE_DELIVERY_ACK       0x0A  // Acknowledge message received
#define MSG_TYPE_DELIVERY_STATUS    0x0B  // Request delivery status update
#define MSG_TYPE_READ_RECEIPT       0x0C  // Message has been read/viewed

// Noise Protocol messages
#define MSG_TYPE_NOISE_HANDSHAKE_INIT  0x10  // Noise handshake initiation
#define MSG_TYPE_NOISE_HANDSHAKE_RESP  0x11  // Noise handshake response
#define MSG_TYPE_NOISE_ENCRYPTED       0x12  // Noise encrypted transport message
#define MSG_TYPE_NOISE_IDENTITY        0x13  // Announce static public key

// Protocol version negotiation
#define MSG_TYPE_VERSION_HELLO      0x20  // Initial version announcement
#define MSG_TYPE_VERSION_ACK        0x21  // Version acknowledgment

// Protocol-level acknowledgments
#define MSG_TYPE_PROTOCOL_ACK       0x22  // Generic protocol acknowledgment
#define MSG_TYPE_PROTOCOL_NACK      0x23  // Negative acknowledgment (failure)
#define MSG_TYPE_SYSTEM_VALIDATION  0x24  // Session validation ping
#define MSG_TYPE_HANDSHAKE_REQUEST  0x25  // Request handshake for pending messages

// Favorite system messages
#define MSG_TYPE_FAVORITED          0x30  // Peer favorited us
#define MSG_TYPE_UNFAVORITED        0x31  // Peer unfavorited us

// Binary Protocol Constants (from BinaryProtocol.swift)
#define HEADER_SIZE                 13
#define SENDER_ID_SIZE              8
#define RECIPIENT_ID_SIZE           8
#define SIGNATURE_SIZE              64

// Protocol Flags (from BinaryProtocol.swift Flags struct)
#define FLAG_HAS_RECIPIENT          0x01
#define FLAG_HAS_SIGNATURE          0x02
#define FLAG_IS_COMPRESSED          0x04

// Default protocol version
#define PROTOCOL_VERSION            3
#define DEFAULT_TTL                 10

// BitchatPacket structure (from BitchatProtocol.swift)
struct BitchatPacket {
    uint8_t version;        // Protocol version
    uint8_t type;           // Message type (from MessageType enum)
    uint8_t* senderID;      // 8-byte sender ID
    uint8_t* recipientID;   // 8-byte recipient ID (optional)
    uint64_t timestamp;     // Unix timestamp
    uint8_t* payload;       // Variable length payload
    uint8_t* signature;     // 64-byte signature (optional)
    uint8_t ttl;           // Time to live
    uint16_t payloadLength; // Payload length
    
    // Constructor
    BitchatPacket() : version(PROTOCOL_VERSION), type(0), senderID(nullptr), 
                     recipientID(nullptr), timestamp(0), payload(nullptr), 
                     signature(nullptr), ttl(DEFAULT_TTL), payloadLength(0) {}
};