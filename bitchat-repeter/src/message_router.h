#pragma once

#include <stdint.h>
#include <map>
#include <vector>
#include "bitchat_protocol.h"

// Message deduplication entry
struct MessageCacheEntry {
    uint32_t messageId;        // Hash-based message ID
    unsigned long timestamp;   // When message was first seen (millis())
    
    MessageCacheEntry() : messageId(0), timestamp(0) {}
    MessageCacheEntry(uint32_t id, unsigned long ts) : messageId(id), timestamp(ts) {}
};

// Circular buffer-based message cache for deduplication
class MessageCache {
private:
    static const size_t CACHE_SIZE = 1000;    // Store 1000 most recent messages
    static const unsigned long EXPIRE_TIME = 300000; // 5 minutes in milliseconds
    
    std::vector<MessageCacheEntry> entries;   // Circular buffer
    std::map<uint32_t, size_t> idToIndex;     // Map message ID to buffer index
    size_t currentIndex;                      // Current position in circular buffer
    size_t totalEntries;                      // Total entries added (for wrapping)
    
public:
    MessageCache();
    bool isDuplicate(uint32_t messageId, unsigned long timestamp);
    void addMessage(uint32_t messageId, unsigned long timestamp);
    void cleanupExpired();
    size_t getSize() const { return totalEntries < CACHE_SIZE ? totalEntries : CACHE_SIZE; }
};

class MessageRouter {
public:
    static void init();
    static void process();
    
    // Message deduplication
    static uint32_t generateMessageId(const BitchatPacket& packet);
    static bool isDuplicate(const BitchatPacket& packet);
    static void recordMessage(const BitchatPacket& packet);
    
    // Message handling
    static void handleBLEMessage(const BitchatPacket& packet, uint16_t connectionHandle);
    static void handleLoRaMessage(const BitchatPacket& packet);
    
private:
    static MessageCache dedupCache;
    static unsigned long lastCleanup;  // Last time we cleaned up expired entries
    
    // Helper functions
    static void performCleanup();
};