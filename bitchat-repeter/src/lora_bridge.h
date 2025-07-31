#pragma once

#include <RadioLib.h>
#include "hardware_config.h"
#include "bitchat_protocol.h"
#include <map>
#include <string>

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
    
    NeighborEntry() : repeaterId(0), connectedDevices(0), queueDepth(0), 
                     capabilities(0), firmwareVersion(1), lastSeen(0), 
                     avgRSSI(0), linkQuality(0), packetCount(0), rssiSum(0) {}
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
    
private:
    static std::map<uint32_t, NeighborEntry> neighbors;
    static const unsigned long NEIGHBOR_TIMEOUT_MS = 300000; // 5 minutes
    static uint8_t calculateLinkQuality(int avgRSSI);
};

class LoRaBridge {
public:
    static void init();
    static void process();
    
    // Message transmission
    static bool transmit(const BitchatPacket& packet);
    static bool transmitLoRaPacket(const LoRaPacket& packet);
    static void startReceive();
    
    // Neighbor discovery
    static void sendNeighborAnnouncement();
    static void handleNeighborAnnouncement(const LoRaPacket& packet, int rssi);
    
    // Status and statistics
    static bool isReceiving();
    static int getLastRSSI();
    static float getLastSNR();
    
private:
    // RadioLib SX1262 instance
    static SX1262 radio;
    
    // Radio state
    static bool initialized;
    static bool receiving;
    static unsigned long lastRxCheck;
    static unsigned long lastNeighborAnnouncement;
    
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
    
    // Interrupt handling
    static volatile bool receivedFlag;
    static void onReceive();
    
    // Helper functions
    static bool configure();
    static void handleReceivedMessage();
    static void handleReceivedLoRaPacket(const uint8_t* buffer, size_t size);
};