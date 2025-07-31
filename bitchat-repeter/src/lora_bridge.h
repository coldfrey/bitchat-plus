#pragma once

#include <RadioLib.h>
#include "hardware_config.h"
#include "bitchat_protocol.h"
#include <map>
#include <string>
#include <queue>

// Transmission queue entry for LoRa packets
struct QueuedPacket {
    LoRaPacket packet;
    unsigned long queueTime;      // When packet was queued
    uint8_t retryCount;           // Number of transmission attempts
    unsigned long nextAttempt;    // When to attempt next transmission
    
    QueuedPacket() : queueTime(0), retryCount(0), nextAttempt(0) {}
    QueuedPacket(const LoRaPacket& pkt) : packet(pkt), queueTime(millis()), 
                                         retryCount(0), nextAttempt(millis()) {}
};

// Neighbor table entry for mesh network discovery
struct NeighborEntry {
    uint32_t repeaterId;        // Neighbor's repeater ID
    std::string name;           // Neighbor's device name
    uint8_t connectedDevices;   // Number of BLE devices connected
    uint8_t queueDepth;         // Current message queue depth
    uint8_t capabilities;       // Capability flags (battery, GPS, etc.)
    uint8_t firmwareVersion;    // Firmware version
    unsigned long lastSeen;     // Timestamp of last NEIGHBOR_ANNOUNCE
    int avgRSSI;               // Rolling average RSSI (last 10 packets)
    uint8_t linkQuality;       // Link quality score (0-100)
    uint8_t packetCount;       // Number of packets received (for averaging)
    int rssiSum;               // Sum for rolling average calculation
    uint8_t optimalSF;         // Optimal spreading factor for this neighbor
    unsigned long lastSFUpdate; // Last time SF was updated
    
    NeighborEntry() : repeaterId(0), connectedDevices(0), queueDepth(0), 
                     capabilities(0), firmwareVersion(1), lastSeen(0), 
                     avgRSSI(0), linkQuality(0), packetCount(0), rssiSum(0),
                     optimalSF(9), lastSFUpdate(0) {}
};

// Capability flags for neighbor announcements
#define NEIGHBOR_CAP_BATTERY_POWERED    0x01  // Running on battery
#define NEIGHBOR_CAP_GPS_EQUIPPED       0x02  // Has GPS capability
#define NEIGHBOR_CAP_HIGH_POWER         0x04  // High power transmitter
#define NEIGHBOR_CAP_ALWAYS_ON          0x08  // Always-on device (mains power)

class NeighborTable {
public:
    static void init();
    static void addOrUpdateNeighbor(uint32_t repeaterId, const std::string& name, 
                                   uint8_t connectedDevices, uint8_t queueDepth, 
                                   uint8_t capabilities, uint8_t firmwareVersion, int rssi);
    static void removeNeighbor(uint32_t repeaterId);
    static void cleanupStaleNeighbors();
    static NeighborEntry* getNeighbor(uint32_t repeaterId);
    static std::map<uint32_t, NeighborEntry>& getAllNeighbors();
    static size_t getNeighborCount();
    static void printNeighborTable();
    
    // Adaptive data rate optimization
    static uint8_t getOptimalSpreadingFactor(uint32_t repeaterId);
    static void updateAdaptiveRates();
    static uint8_t calculateOptimalSF(int avgRSSI);
    
private:
    static std::map<uint32_t, NeighborEntry> neighbors;
    static const unsigned long NEIGHBOR_TIMEOUT_MS = 300000; // 5 minutes
    static const unsigned long SF_UPDATE_INTERVAL_MS = 30000; // Update SF every 30 seconds
    static uint8_t calculateLinkQuality(int avgRSSI);
};

// Route table entry for mesh routing
struct RouteEntry {
    uint32_t destination;       // Destination repeater ID
    uint32_t nextHop;          // Next hop repeater ID
    uint8_t hopCount;          // Number of hops to destination
    uint32_t sequenceNumber;   // Sequence number for freshness
    unsigned long lastUsed;    // Last time route was used
    unsigned long expiry;      // When route expires
    bool isValid;             // Route validity flag
    uint8_t routeQuality;      // Combined route quality score (0-100)
    uint8_t loadFactor;        // Load factor of next hop (0-100)
    
    RouteEntry() : destination(0), nextHop(0), hopCount(255), sequenceNumber(0),
                  lastUsed(0), expiry(0), isValid(false), routeQuality(0), loadFactor(0) {}
    
    RouteEntry(uint32_t dest, uint32_t next, uint8_t hops, uint32_t seqNum) :
        destination(dest), nextHop(next), hopCount(hops), sequenceNumber(seqNum),
        lastUsed(millis()), expiry(millis() + 600000), isValid(true), routeQuality(0), loadFactor(0) {} // 10-minute timeout
};

// Route request entry for tracking ongoing route discoveries
struct RouteRequestEntry {
    uint32_t requestId;        // Unique request ID
    uint32_t originator;       // Original requester
    uint32_t destination;      // Target destination
    unsigned long timestamp;   // When request was initiated
    bool replied;             // Whether we've replied to this request
    
    RouteRequestEntry() : requestId(0), originator(0), destination(0), 
                         timestamp(0), replied(false) {}
    
    RouteRequestEntry(uint32_t reqId, uint32_t orig, uint32_t dest) :
        requestId(reqId), originator(orig), destination(dest),
        timestamp(millis()), replied(false) {}
};

class RouteTable {
public:
    static void init();
    static void addRoute(uint32_t destination, uint32_t nextHop, uint8_t hopCount, uint32_t sequenceNumber);
    static RouteEntry* findRoute(uint32_t destination);
    static bool removeRoute(uint32_t destination);
    static void cleanupExpiredRoutes();
    static void printRouteTable();
    static size_t getRouteCount();
    
    // Route discovery management
    static bool isRouteRequestPending(uint32_t destination);
    static void addRouteRequest(uint32_t requestId, uint32_t originator, uint32_t destination);
    static RouteRequestEntry* findRouteRequest(uint32_t requestId);
    static void cleanupExpiredRequests();
    static uint32_t generateRequestId();
    
    // Load balancing and optimization
    static RouteEntry* findBestRoute(uint32_t destination);
    static void updateRouteMetrics();
    static uint8_t calculateRouteScore(const RouteEntry& route);
    static uint8_t calculateLoadFactor(uint32_t nextHop);
    static bool isPowerAwareRoute(uint32_t nextHop);
    
private:
    static std::map<uint32_t, RouteEntry> routes;
    static std::map<uint32_t, RouteRequestEntry> pendingRequests;
    static const unsigned long ROUTE_TIMEOUT_MS = 600000;      // 10 minutes
    static const unsigned long REQUEST_TIMEOUT_MS = 30000;     // 30 seconds
    static uint32_t nextRequestId;
};

// Pending acknowledgment entry for reliable delivery
struct PendingAckEntry {
    uint32_t packetId;          // Unique packet identifier
    uint32_t destination;       // Next hop destination expecting ACK
    LoRaPacket originalPacket;  // Original packet for retransmission
    unsigned long sentTime;     // When packet was sent
    unsigned long nextRetry;    // When to retry transmission
    uint8_t retryCount;         // Number of retries attempted
    bool waitingForAck;         // Whether we're waiting for ACK
    
    PendingAckEntry() : packetId(0), destination(0), sentTime(0), nextRetry(0),
                       retryCount(0), waitingForAck(false) {}
    
    PendingAckEntry(uint32_t id, uint32_t dest, const LoRaPacket& pkt) :
        packetId(id), destination(dest), originalPacket(pkt), sentTime(millis()),
        nextRetry(millis() + 100), retryCount(0), waitingForAck(true) {}
};

// Mesh deduplication entry
struct MeshDedupeEntry {
    uint32_t sourceRepeater;    // Original source repeater
    uint32_t sequenceNumber;    // Sequence number from source
    unsigned long timestamp;    // When we first saw this packet
    
    MeshDedupeEntry() : sourceRepeater(0), sequenceNumber(0), timestamp(0) {}
    MeshDedupeEntry(uint32_t src, uint32_t seq) :
        sourceRepeater(src), sequenceNumber(seq), timestamp(millis()) {}
};

// Alternative route entry for backup paths
struct AlternativeRoute {
    uint32_t destination;       // Destination repeater
    uint32_t nextHop;          // Alternative next hop
    uint8_t hopCount;          // Hop count via this path
    uint32_t sequenceNumber;   // Route sequence number
    unsigned long lastUsed;    // Last time this route was used
    uint8_t priority;          // Route priority (0=primary, 1=backup, etc.)
    
    AlternativeRoute() : destination(0), nextHop(0), hopCount(255),
                        sequenceNumber(0), lastUsed(0), priority(1) {}
    
    AlternativeRoute(uint32_t dest, uint32_t next, uint8_t hops, uint32_t seq, uint8_t prio) :
        destination(dest), nextHop(next), hopCount(hops), sequenceNumber(seq),
        lastUsed(millis()), priority(prio) {}
};

class ReliabilityManager {
public:
    static void init();
    static uint32_t generatePacketId();
    static void addPendingAck(uint32_t packetId, uint32_t destination, const LoRaPacket& packet);
    static void handleMeshAck(uint32_t packetId, uint32_t source);
    static void processAckTimeouts();
    static void cleanupExpiredEntries();
    
    // Mesh deduplication
    static bool isDuplicatePacket(uint32_t sourceRepeater, uint32_t sequenceNumber);
    static void addSeenPacket(uint32_t sourceRepeater, uint32_t sequenceNumber);
    static void cleanupDedupeEntries();
    
    // Alternative routes
    static void addAlternativeRoute(uint32_t destination, uint32_t nextHop, uint8_t hopCount, uint32_t sequenceNumber, uint8_t priority);
    static AlternativeRoute* getAlternativeRoute(uint32_t destination, uint32_t failedNextHop);
    static void markRouteFailed(uint32_t destination, uint32_t nextHop);
    
private:
    static std::map<uint32_t, PendingAckEntry> pendingAcks;
    static std::map<uint64_t, MeshDedupeEntry> meshDedupeCache; // Key: (source << 32) | sequence
    static std::map<uint32_t, std::vector<AlternativeRoute>> alternativeRoutes;
    static uint32_t nextPacketId;
    static const unsigned long ACK_TIMEOUT_MS = 1000;          // 1 second
    static const unsigned long DEDUPE_TIMEOUT_MS = 300000;     // 5 minutes
    static const uint8_t MAX_RETRIES = 3;
};

class LoRaBridge {
public:
    static void init();
    static void process();
    
    // Message transmission
    static bool transmit(const BitchatPacket& packet);
    static bool transmitLoRaPacket(const LoRaPacket& packet);
    static bool queueLoRaPacket(const LoRaPacket& packet);
    static void startReceive();
    
    // Neighbor discovery
    static void sendNeighborAnnouncement();
    static void handleNeighborAnnouncement(const LoRaPacket& packet, int rssi);
    
    // Mesh routing
    static bool routeDataPacket(const LoRaPacket& packet);
    static void initiateRouteDiscovery(uint32_t destination);
    static void handleRouteRequest(const LoRaPacket& packet, int rssi);
    static void handleRouteReply(const LoRaPacket& packet, int rssi);
    static void sendRouteRequest(uint32_t destination);
    static void sendRouteReply(uint32_t destination, uint32_t originator, uint32_t requestId, uint8_t hopCount);
    
    // Reliable delivery
    static bool sendReliablePacket(const LoRaPacket& packet, uint32_t nextHop);
    static void handleMeshAck(const LoRaPacket& packet, int rssi);
    static void sendMeshAck(uint32_t packetId, uint32_t destination);
    static void processReliabilityTimeouts();
    
    // Status and statistics
    static bool isReceiving();
    static int getLastRSSI();
    static float getLastSNR();
    
    // Adaptive transmission optimization
    static bool transmitAdaptivePacket(const LoRaPacket& packet, uint32_t targetRepeater);
    static void updateNetworkOptimization();
    
    // Public constants for optimization algorithms
    static const int RSSI_THRESHOLD_STRONG = -80;                     // Strong link threshold
    static const int RSSI_THRESHOLD_MEDIUM = -100;                    // Medium link threshold
    static const uint8_t SF_STRONG_LINK = 7;                          // SF for strong links
    static const uint8_t SF_MEDIUM_LINK = 9;                          // SF for medium links  
    static const uint8_t SF_WEAK_LINK = 10;                           // SF for weak links
    
private:
    // RadioLib SX1262 instance
    static SX1262 radio;
    
    // Radio state
    static bool initialized;
    static bool receiving;
    static unsigned long lastRxCheck;
    static unsigned long lastNeighborAnnouncement;
    
    // Transmission queue and collision avoidance
    static std::queue<QueuedPacket> transmissionQueue;
    static unsigned long lastTransmission;
    static bool channelBusy;
    
    // Duty cycle tracking (1% per hour = 36 seconds)
    static unsigned long dutyCycleStartTime;
    static unsigned long totalAirTimeMs;
    static const unsigned long DUTY_CYCLE_WINDOW_MS = 3600000; // 1 hour
    static const unsigned long MAX_AIRTIME_MS = 36000;         // 36 seconds per hour
    
    // Routing state
    static uint32_t ownSequenceNumber;
    
    // Adaptive optimization state
    static unsigned long lastOptimizationUpdate;
    static uint8_t currentSpreadingFactor;
    static bool adaptiveRatesEnabled;
    
    // Statistics
    static int lastRSSI;
    static float lastSNR;
    static unsigned long txCount;
    static unsigned long rxCount;
    
    // Configuration constants
    static const float FREQUENCY;
    static const float BANDWIDTH;
    static const uint8_t SPREADING_FACTOR;
    static const uint8_t CODING_RATE;
    static const int8_t TX_POWER;
    static const uint8_t SYNC_WORD;
    
    // Neighbor discovery constants
    static const unsigned long NEIGHBOR_ANNOUNCE_INTERVAL_MS = 60000;  // 60 seconds
    
    // Transmission constants
    static const size_t MAX_QUEUE_SIZE = 20;                          // Maximum queued packets
    static const unsigned long CAD_TIMEOUT_MS = 100;                  // CAD detection timeout
    static const unsigned long MIN_BACKOFF_MS = 50;                   // Minimum backoff time
    static const unsigned long MAX_BACKOFF_MS = 400;                  // Maximum backoff time
    static const uint8_t MAX_RETRIES = 3;                            // Maximum transmission retries
    
    // Optimization constants
    static const unsigned long OPTIMIZATION_UPDATE_INTERVAL_MS = 30000; // 30 seconds
    
    // Interrupt handling
    static volatile bool receivedFlag;
    static void onReceive();
    
    // Helper functions
    static bool configure();
    static void handleReceivedMessage();
    static void handleReceivedLoRaPacket(const uint8_t* buffer, size_t size);
    
    // Transmission helpers
    static void processTransmissionQueue();
    static bool isChannelClear();
    static unsigned long calculateAirTime(size_t packetSize);
    static bool isDutyCycleExceeded(unsigned long airTimeMs);
    static void updateDutyCycle(unsigned long airTimeMs);
    static unsigned long getRandomBackoff(uint8_t retryCount);
};