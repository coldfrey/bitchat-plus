#pragma once

#include <map>
#include <Arduino.h>

// Connection quality metrics for BLE connections
struct ConnectionQuality {
    int rssi;                      // Current RSSI value
    int avgRSSI;                   // Rolling average RSSI (last 10 readings)
    int rssiSum;                   // Sum for rolling average calculation
    uint8_t rssiCount;             // Number of RSSI readings
    unsigned long messageCount;    // Total messages sent/received
    unsigned long errorCount;      // Number of transmission errors
    unsigned long connectionTime;  // When connection was established
    unsigned long lastActivity;    // Last message activity timestamp
    unsigned long lastRSSIUpdate;  // Last RSSI measurement timestamp
    uint8_t connectionScore;       // Calculated connection quality score (0-100)
    
    ConnectionQuality() : rssi(0), avgRSSI(0), rssiSum(0), rssiCount(0), 
                         messageCount(0), errorCount(0), connectionTime(0),
                         lastActivity(0), lastRSSIUpdate(0), connectionScore(50) {}
};

// Connection stability tracking for hysteresis
struct ConnectionStability {
    uint8_t lastScore;            // Previous connection score
    unsigned long scoreChangeTime; // When score last changed significantly
    uint8_t stabilityCounter;     // Counter for stability validation
    bool isStable;               // Whether connection is considered stable
    
    ConnectionStability() : lastScore(50), scoreChangeTime(0), stabilityCounter(0), isStable(true) {}
};

class ConnectionManager {
public:
    static void init();
    static void process();
    
    // Connection tracking
    static void addConnection(uint16_t connectionHandle, const String& peerID);
    static void removeConnection(uint16_t connectionHandle);
    static void updateConnectionRSSI(uint16_t connectionHandle, int rssi);
    static void updateConnectionActivity(uint16_t connectionHandle, bool success);
    
    // Connection quality assessment
    static uint8_t getConnectionScore(uint16_t connectionHandle);
    static ConnectionQuality* getConnectionQuality(uint16_t connectionHandle);
    static bool isConnectionHealthy(uint16_t connectionHandle);
    static bool shouldMaintainConnection(uint16_t connectionHandle);
    
    // Network optimization coordination
    static void updateMeshCoordination();
    static uint8_t getBestConnectionScore();
    static size_t getHealthyConnectionCount();
    
    // Statistics and monitoring
    static void printConnectionStats();
    static void cleanupStaleConnections();
    
private:
    static std::map<uint16_t, ConnectionQuality> connectionQualities;
    static std::map<uint16_t, ConnectionStability> connectionStabilities;
    static std::map<uint16_t, String> connectionPeerIDs;
    
    // Configuration constants
    static const unsigned long RSSI_UPDATE_INTERVAL_MS = 5000;      // 5 seconds
    static const unsigned long ACTIVITY_TIMEOUT_MS = 300000;       // 5 minutes
    static const unsigned long CLEANUP_INTERVAL_MS = 60000;        // 1 minute
    static const unsigned long STABILITY_WINDOW_MS = 30000;        // 30 seconds
    static const uint8_t STABILITY_THRESHOLD = 10;                 // Score change threshold
    static const uint8_t MIN_STABILITY_COUNT = 3;                  // Minimum stable readings
    
    // Quality thresholds
    static const int RSSI_EXCELLENT = -50;    // Excellent signal strength
    static const int RSSI_GOOD = -70;         // Good signal strength  
    static const int RSSI_FAIR = -85;         // Fair signal strength
    static const int RSSI_POOR = -100;        // Poor signal strength
    static const uint8_t ERROR_RATE_GOOD = 5; // 5% error rate threshold
    static const uint8_t ERROR_RATE_POOR = 20; // 20% error rate threshold
    
    // Helper functions
    static uint8_t calculateConnectionScore(const ConnectionQuality& quality);
    static void updateConnectionStability(uint16_t connectionHandle, uint8_t newScore);
    static uint8_t calculateRSSIScore(int avgRSSI);
    static uint8_t calculateErrorRateScore(const ConnectionQuality& quality);
    static uint8_t calculateDurationScore(unsigned long connectionTime);
    static bool hasStableScore(uint16_t connectionHandle, uint8_t newScore);
    
    // Mesh coordination
    static unsigned long lastMeshUpdate;
    static const unsigned long MESH_UPDATE_INTERVAL_MS = 15000;     // 15 seconds
};