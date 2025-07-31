#include "core/connection_manager.h"
#include "communication/lora_bridge.h"
#include <Arduino.h>

// Static member definitions
std::map<uint16_t, ConnectionQuality> ConnectionManager::connectionQualities;
std::map<uint16_t, ConnectionStability> ConnectionManager::connectionStabilities;
std::map<uint16_t, String> ConnectionManager::connectionPeerIDs;
unsigned long ConnectionManager::lastMeshUpdate = 0;

// Configuration constants
const unsigned long ConnectionManager::RSSI_UPDATE_INTERVAL_MS;
const unsigned long ConnectionManager::ACTIVITY_TIMEOUT_MS;
const unsigned long ConnectionManager::CLEANUP_INTERVAL_MS;
const unsigned long ConnectionManager::STABILITY_WINDOW_MS;
const uint8_t ConnectionManager::STABILITY_THRESHOLD;
const uint8_t ConnectionManager::MIN_STABILITY_COUNT;
const int ConnectionManager::RSSI_EXCELLENT;
const int ConnectionManager::RSSI_GOOD;
const int ConnectionManager::RSSI_FAIR;
const int ConnectionManager::RSSI_POOR;
const uint8_t ConnectionManager::ERROR_RATE_GOOD;
const uint8_t ConnectionManager::ERROR_RATE_POOR;
const unsigned long ConnectionManager::MESH_UPDATE_INTERVAL_MS;

void ConnectionManager::init() {
    connectionQualities.clear();
    connectionStabilities.clear();
    connectionPeerIDs.clear();
    lastMeshUpdate = 0;
    Serial.println("ConnectionManager: Initialized");
}

void ConnectionManager::process() {
    unsigned long now = millis();
    
    // Clean up stale connections periodically
    static unsigned long lastCleanup = 0;
    if (now - lastCleanup > CLEANUP_INTERVAL_MS) {
        lastCleanup = now;
        cleanupStaleConnections();
    }
    
    // Update mesh coordination periodically
    if (now - lastMeshUpdate > MESH_UPDATE_INTERVAL_MS) {
        lastMeshUpdate = now;
        updateMeshCoordination();
    }
    
    // Update connection scores for all active connections
    for (auto& pair : connectionQualities) {
        uint16_t connectionHandle = pair.first;
        ConnectionQuality& quality = pair.second;
        
        // Calculate and update connection score
        uint8_t newScore = calculateConnectionScore(quality);
        updateConnectionStability(connectionHandle, newScore);
        quality.connectionScore = newScore;
    }
}

void ConnectionManager::addConnection(uint16_t connectionHandle, const String& peerID) {
    unsigned long now = millis();
    
    ConnectionQuality quality;
    quality.connectionTime = now;
    quality.lastActivity = now;
    
    ConnectionStability stability;
    stability.scoreChangeTime = now;
    
    connectionQualities[connectionHandle] = quality;
    connectionStabilities[connectionHandle] = stability;
    connectionPeerIDs[connectionHandle] = peerID;
    
    Serial.printf("ConnectionManager: Added connection %d for peer %s\n", connectionHandle, peerID.c_str());
}

void ConnectionManager::removeConnection(uint16_t connectionHandle) {
    auto qualityIt = connectionQualities.find(connectionHandle);
    if (qualityIt != connectionQualities.end()) {
        String peerID = connectionPeerIDs[connectionHandle];
        Serial.printf("ConnectionManager: Removed connection %d for peer %s\n", connectionHandle, peerID.c_str());
        
        connectionQualities.erase(qualityIt);
        connectionStabilities.erase(connectionHandle);
        connectionPeerIDs.erase(connectionHandle);
    }
}

void ConnectionManager::updateConnectionRSSI(uint16_t connectionHandle, int rssi) {
    auto it = connectionQualities.find(connectionHandle);
    if (it != connectionQualities.end()) {
        ConnectionQuality& quality = it->second;
        quality.rssi = rssi;
        quality.lastRSSIUpdate = millis();
        
        // Update rolling average RSSI (similar to NeighborTable)
        if (quality.rssiCount < 10) {
            quality.rssiSum += rssi;
            quality.rssiCount++;
        } else {
            // Rolling average of last 10 readings
            quality.rssiSum = quality.rssiSum - quality.avgRSSI + rssi;
            quality.rssiCount = 10;
        }
        quality.avgRSSI = quality.rssiSum / quality.rssiCount;
        
        Serial.printf("ConnectionManager: Updated RSSI for connection %d: %d dBm (avg: %d dBm)\n",
                     connectionHandle, rssi, quality.avgRSSI);
    }
}

void ConnectionManager::updateConnectionActivity(uint16_t connectionHandle, bool success) {
    auto it = connectionQualities.find(connectionHandle);
    if (it != connectionQualities.end()) {
        ConnectionQuality& quality = it->second;
        quality.messageCount++;
        quality.lastActivity = millis();
        
        if (!success) {
            quality.errorCount++;
        }
        
        // Calculate error rate for logging
        uint8_t errorRate = (quality.messageCount > 0) ? 
            (quality.errorCount * 100 / quality.messageCount) : 0;
        
        if (!success) {
            Serial.printf("ConnectionManager: Message error on connection %d (error rate: %d%%)\n",
                         connectionHandle, errorRate);
        }
    }
}

uint8_t ConnectionManager::getConnectionScore(uint16_t connectionHandle) {
    auto it = connectionQualities.find(connectionHandle);
    if (it != connectionQualities.end()) {
        return it->second.connectionScore;
    }
    return 0; // No connection found
}

ConnectionQuality* ConnectionManager::getConnectionQuality(uint16_t connectionHandle) {
    auto it = connectionQualities.find(connectionHandle);
    if (it != connectionQualities.end()) {
        return &it->second;
    }
    return nullptr;
}

bool ConnectionManager::isConnectionHealthy(uint16_t connectionHandle) {
    uint8_t score = getConnectionScore(connectionHandle);
    return score >= 60; // Consider connections with 60%+ score as healthy
}

bool ConnectionManager::shouldMaintainConnection(uint16_t connectionHandle) {
    auto stabIt = connectionStabilities.find(connectionHandle);
    if (stabIt != connectionStabilities.end()) {
        ConnectionStability& stability = stabIt->second;
        
        // Use hysteresis: require connection to be stable and have decent score
        if (stability.isStable) {
            return getConnectionScore(connectionHandle) >= 40; // Lower threshold for stable connections
        } else {
            return getConnectionScore(connectionHandle) >= 70; // Higher threshold for unstable connections
        }
    }
    return false;
}

void ConnectionManager::updateMeshCoordination() {
    // Share connection statistics with mesh network for optimization
    size_t healthyConnections = getHealthyConnectionCount();
    uint8_t bestScore = getBestConnectionScore();
    
    Serial.printf("ConnectionManager: Mesh coordination - %d healthy connections, best score: %d%%\n",
                 healthyConnections, bestScore);
    
    // TODO: Could integrate with LoRaBridge neighbor announcements to share load info
    // This would help other repeaters make better routing decisions
}

uint8_t ConnectionManager::getBestConnectionScore() {
    uint8_t bestScore = 0;
    for (const auto& pair : connectionQualities) {
        if (pair.second.connectionScore > bestScore) {
            bestScore = pair.second.connectionScore;
        }
    }
    return bestScore;
}

size_t ConnectionManager::getHealthyConnectionCount() {
    size_t count = 0;
    for (const auto& pair : connectionQualities) {
        if (pair.second.connectionScore >= 60) {
            count++;
        }
    }
    return count;
}

void ConnectionManager::printConnectionStats() {
    Serial.printf("=== Connection Manager Stats (%d connections) ===\n", connectionQualities.size());
    Serial.println("Handle PeerID           RSSI  Avg   Score Msgs  Errs  Duration Stable");
    Serial.println("------ --------------- ----- ----- ----- ----- ----- -------- ------");
    
    unsigned long now = millis();
    for (const auto& pair : connectionQualities) {
        uint16_t handle = pair.first;
        const ConnectionQuality& quality = pair.second;
        const ConnectionStability& stability = connectionStabilities.at(handle);
        const String& peerID = connectionPeerIDs.at(handle);
        
        unsigned long duration = (now - quality.connectionTime) / 1000; // seconds
        uint8_t errorRate = (quality.messageCount > 0) ? 
            (quality.errorCount * 100 / quality.messageCount) : 0;
        
        Serial.printf("%-6d %-15s %-5d %-5d %-5d %-5lu %-4d%% %-8lu %s\n",
                     handle, peerID.c_str(), quality.rssi, quality.avgRSSI, 
                     quality.connectionScore, quality.messageCount, errorRate,
                     duration, stability.isStable ? "Yes" : "No");
    }
    Serial.println("==================================================");
}

void ConnectionManager::cleanupStaleConnections() {
    unsigned long now = millis();
    auto it = connectionQualities.begin();
    int cleanedCount = 0;
    
    while (it != connectionQualities.end()) {
        uint16_t handle = it->first;
        const ConnectionQuality& quality = it->second;
        
        // Remove connections with no activity for extended period
        if (now - quality.lastActivity > ACTIVITY_TIMEOUT_MS) {
            String peerID = connectionPeerIDs[handle];
            Serial.printf("ConnectionManager: Cleaning up stale connection %d for peer %s\n",
                         handle, peerID.c_str());
            
            connectionStabilities.erase(handle);
            connectionPeerIDs.erase(handle);
            it = connectionQualities.erase(it);
            cleanedCount++;
        } else {
            ++it;
        }
    }
    
    if (cleanedCount > 0) {
        Serial.printf("ConnectionManager: Cleaned up %d stale connections\n", cleanedCount);
    }
}

uint8_t ConnectionManager::calculateConnectionScore(const ConnectionQuality& quality) {
    // Multi-factor scoring algorithm
    uint8_t rssiScore = calculateRSSIScore(quality.avgRSSI);
    uint8_t errorScore = calculateErrorRateScore(quality);
    uint8_t durationScore = calculateDurationScore(quality.connectionTime);
    
    // Weighted average: RSSI 50%, Error Rate 30%, Duration 20%
    uint8_t combinedScore = (rssiScore * 50 + errorScore * 30 + durationScore * 20) / 100;
    
    return combinedScore;
}

void ConnectionManager::updateConnectionStability(uint16_t connectionHandle, uint8_t newScore) {
    auto it = connectionStabilities.find(connectionHandle);
    if (it != connectionStabilities.end()) {
        ConnectionStability& stability = it->second;
        unsigned long now = millis();
        
        // Check if score changed significantly
        uint8_t scoreDiff = (newScore > stability.lastScore) ? 
            (newScore - stability.lastScore) : (stability.lastScore - newScore);
        
        if (scoreDiff >= STABILITY_THRESHOLD) {
            // Significant change - reset stability tracking
            stability.scoreChangeTime = now;
            stability.stabilityCounter = 0;
            stability.isStable = false;
            Serial.printf("ConnectionManager: Connection %d score changed significantly (%d -> %d)\n",
                         connectionHandle, stability.lastScore, newScore);
        } else if (!stability.isStable && now - stability.scoreChangeTime > STABILITY_WINDOW_MS) {
            // Score has been stable for the required window
            stability.stabilityCounter++;
            if (stability.stabilityCounter >= MIN_STABILITY_COUNT) {
                stability.isStable = true;
                Serial.printf("ConnectionManager: Connection %d is now stable (score: %d)\n",
                             connectionHandle, newScore);
            }
        }
        
        stability.lastScore = newScore;
    }
}

uint8_t ConnectionManager::calculateRSSIScore(int avgRSSI) {
    // RSSI-based scoring similar to NeighborTable
    if (avgRSSI >= RSSI_EXCELLENT) {
        return 100;
    } else if (avgRSSI >= RSSI_GOOD) {
        return 80 + ((avgRSSI - RSSI_GOOD) * 20 / (RSSI_EXCELLENT - RSSI_GOOD));
    } else if (avgRSSI >= RSSI_FAIR) {
        return 60 + ((avgRSSI - RSSI_FAIR) * 20 / (RSSI_GOOD - RSSI_FAIR));
    } else if (avgRSSI >= RSSI_POOR) {
        return 30 + ((avgRSSI - RSSI_POOR) * 30 / (RSSI_FAIR - RSSI_POOR));
    } else {
        return 0;
    }
}

uint8_t ConnectionManager::calculateErrorRateScore(const ConnectionQuality& quality) {
    if (quality.messageCount == 0) {
        return 100; // No messages = no errors
    }
    
    uint8_t errorRate = (quality.errorCount * 100) / quality.messageCount;
    
    if (errorRate <= ERROR_RATE_GOOD) {
        return 100;
    } else if (errorRate <= ERROR_RATE_POOR) {
        return 100 - ((errorRate - ERROR_RATE_GOOD) * 70 / (ERROR_RATE_POOR - ERROR_RATE_GOOD));
    } else {
        return 30; // Very high error rate = minimum score
    }
}

uint8_t ConnectionManager::calculateDurationScore(unsigned long connectionTime) {
    unsigned long now = millis();
    unsigned long durationMinutes = (now - connectionTime) / 60000; // minutes
    
    // Reward longer connections (up to 60 minutes for full score)
    if (durationMinutes >= 60) {
        return 100;
    } else if (durationMinutes >= 5) {
        return 60 + (durationMinutes - 5) * 40 / 55; // Linear scale from 5-60 minutes
    } else {
        return 60; // New connections get medium score initially
    }
}

bool ConnectionManager::hasStableScore(uint16_t connectionHandle, uint8_t newScore) {
    auto it = connectionStabilities.find(connectionHandle);
    if (it != connectionStabilities.end()) {
        return it->second.isStable;
    }
    return false;
}