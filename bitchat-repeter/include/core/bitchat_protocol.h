#pragma once

#include <stdint.h>
#include <string.h>

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

// Default protocol version (compatible with iOS BitChat app)
#define PROTOCOL_VERSION            1
#define DEFAULT_TTL                 10

// Simplified BitchatPacket structure for v1 implementation
struct BitchatPacket {
    uint8_t version;        // Protocol version
    uint8_t type;           // Message type (from MessageType enum)
    uint8_t senderID[8];    // 8-byte sender ID (fixed array)
    uint8_t recipientID[8]; // 8-byte recipient ID (all zeros for broadcast)
    uint64_t timestamp;     // Unix timestamp in milliseconds
    uint8_t ttl;           // Time to live (hop count)
    uint16_t payloadLength; // Payload length
    uint8_t* payload;       // Variable length payload (points to external buffer)
    
    // Constructor
    BitchatPacket() : version(PROTOCOL_VERSION), type(0), timestamp(0), 
                     ttl(DEFAULT_TTL), payloadLength(0), payload(nullptr) {
        memset(senderID, 0, 8);
        memset(recipientID, 0, 8);
    }
};

// Packet parsing results
enum ParseResult {
    PARSE_SUCCESS = 0,
    PARSE_TOO_SMALL = 1,
    PARSE_INVALID_VERSION = 2,
    PARSE_UNSUPPORTED_TYPE = 3,
    PARSE_INCOMPLETE = 4
};

// LoRa Mesh Packet Format (for repeater-to-repeater communication)
#define LORA_MAGIC                  0xBC
#define LORA_VERSION                0x01
#define LORA_MAX_PAYLOAD_SIZE       220
#define LORA_HEADER_SIZE            20  // Fixed header size without payload

// LoRa Mesh Packet Types
#define LORA_PKT_NEIGHBOR_ANNOUNCE  0x01  // Neighbor discovery
#define LORA_PKT_ROUTE_REQUEST      0x02  // Route discovery request
#define LORA_PKT_ROUTE_REPLY        0x03  // Route discovery reply
#define LORA_PKT_DATA               0x04  // Data packet (contains BitChat packet)
#define LORA_PKT_MESH_ACK           0x05  // Mesh acknowledgment

// Special destination values
#define LORA_DEST_BROADCAST         0xFFFFFFFF

// Default mesh parameters
#define LORA_DEFAULT_MAX_HOPS       5
#define LORA_MAX_HOPS_LIMIT         10

// LoRa packet structure for mesh communication
struct LoRaPacket {
    // Basic header
    uint8_t magic;              // 0xBC - packet magic number
    uint8_t version;            // 0x01 - packet format version
    uint8_t type;               // Mesh packet type (LORA_PKT_*)
    
    // Mesh routing info
    uint32_t srcRepeater;       // Original repeater ID (source)
    uint32_t destRepeater;      // Target repeater ID (0xFFFFFFFF = broadcast)
    uint32_t nextHop;           // Next hop repeater ID
    uint8_t hopCount;           // Number of hops traveled
    uint8_t maxHops;            // Maximum hops allowed (TTL for mesh)
    uint16_t seqNum;            // Sequence number for deduplication
    
    // Payload
    uint8_t payloadLen;         // Length of BitChat packet payload
    uint8_t payload[LORA_MAX_PAYLOAD_SIZE]; // BitChat packet data
    
    // Constructor
    LoRaPacket() : magic(LORA_MAGIC), version(LORA_VERSION), type(0),
                   srcRepeater(0), destRepeater(LORA_DEST_BROADCAST), nextHop(0),
                   hopCount(0), maxHops(LORA_DEFAULT_MAX_HOPS), seqNum(0), payloadLen(0) {
        memset(payload, 0, LORA_MAX_PAYLOAD_SIZE);
    }
};

// LoRa packet parsing results
enum LoRaParseResult {
    LORA_PARSE_SUCCESS = 0,
    LORA_PARSE_TOO_SMALL = 1,
    LORA_PARSE_INVALID_MAGIC = 2,
    LORA_PARSE_INVALID_VERSION = 3,
    LORA_PARSE_UNSUPPORTED_TYPE = 4,
    LORA_PARSE_PAYLOAD_TOO_LARGE = 5
};

// Function declarations for packet handling
ParseResult parsePacket(const uint8_t* data, size_t length, BitchatPacket& packet);
size_t serializePacket(const BitchatPacket& packet, uint8_t* buffer, size_t bufferSize);
bool isSupportedMessageType(uint8_t type);

// LoRa packet handling functions
LoRaParseResult parseLoRaPacket(const uint8_t* data, size_t length, LoRaPacket& packet);
size_t serializeLoRaPacket(const LoRaPacket& packet, uint8_t* buffer, size_t bufferSize);
bool isSupportedLoRaPacketType(uint8_t type);
uint32_t getRepeaterID();  // Generate repeater ID from MAC address