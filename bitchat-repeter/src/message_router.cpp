#include "message_router.h"
#include <Arduino.h>

// Static member definitions
MessageCache MessageRouter::dedupCache;
unsigned long MessageRouter::lastCleanup = 0;

// MessageCache implementation
MessageCache::MessageCache() : currentIndex(0), totalEntries(0) {
    entries.reserve(CACHE_SIZE);
    // Initialize empty entries
    for (size_t i = 0; i < CACHE_SIZE; i++) {
        entries.push_back(MessageCacheEntry());
    }
}

bool MessageCache::isDuplicate(uint32_t messageId, unsigned long timestamp) {
    // Check if message ID exists in our hash map
    auto it = idToIndex.find(messageId);
    if (it == idToIndex.end()) {
        return false; // Not found, not a duplicate
    }
    
    size_t index = it->second;
    MessageCacheEntry& entry = entries[index];
    
    // Verify the entry is still valid (not overwritten)
    if (entry.messageId != messageId) {
        // Entry was overwritten, remove from map and return not duplicate
        idToIndex.erase(it);
        return false;
    }
    
    // Check if entry has expired
    if (timestamp - entry.timestamp > EXPIRE_TIME) {
        // Expired, remove and return not duplicate
        idToIndex.erase(it);
        entry.messageId = 0; // Mark as empty
        return false;
    }
    
    return true; // Found and still valid - duplicate!
}

void MessageCache::addMessage(uint32_t messageId, unsigned long timestamp) {
    // Remove old entry from map if we're about to overwrite it
    MessageCacheEntry& oldEntry = entries[currentIndex];
    if (oldEntry.messageId != 0) {
        idToIndex.erase(oldEntry.messageId);
    }
    
    // Add new entry
    entries[currentIndex] = MessageCacheEntry(messageId, timestamp);
    idToIndex[messageId] = currentIndex;
    
    // Update circular buffer position
    currentIndex = (currentIndex + 1) % CACHE_SIZE;
    totalEntries++;
    
    Serial.printf("Message Cache: Added message ID 0x%08X (cache size: %d)\n", 
                 messageId, getSize());
}

void MessageCache::cleanupExpired() {
    unsigned long now = millis();
    size_t removed = 0;
    
    // Iterate through all entries and remove expired ones
    for (size_t i = 0; i < CACHE_SIZE; i++) {
        MessageCacheEntry& entry = entries[i];
        if (entry.messageId != 0 && (now - entry.timestamp > EXPIRE_TIME)) {
            idToIndex.erase(entry.messageId);
            entry.messageId = 0; // Mark as empty
            removed++;
        }
    }
    
    if (removed > 0) {
        Serial.printf("Message Cache: Cleaned up %d expired entries\n", removed);
    }
}

// MessageRouter implementation
void MessageRouter::init() {
    Serial.println("Message Router: Initializing...");
    lastCleanup = millis();
    Serial.printf("Message Router: Deduplication cache initialized (capacity: %d messages)\n", 1000);
}

void MessageRouter::process() {
    // Perform periodic cleanup every 30 seconds
    unsigned long now = millis();
    if (now - lastCleanup > 30000) {
        performCleanup();
        lastCleanup = now;
    }
}

uint32_t MessageRouter::generateMessageId(const BitchatPacket& packet) {
    // Generate a hash-based message ID from packet content
    // Use a simple hash combining multiple packet fields
    uint32_t hash = 0x811C9DC5; // FNV-1a initial value
    
    // Hash the message type
    hash ^= packet.type;
    hash *= 0x01000193; // FNV-1a prime
    
    // Hash the sender ID
    for (int i = 0; i < 8; i++) {
        hash ^= packet.senderID[i];
        hash *= 0x01000193;
    }
    
    // Hash the recipient ID
    for (int i = 0; i < 8; i++) {
        hash ^= packet.recipientID[i];
        hash *= 0x01000193;
    }
    
    // Hash the timestamp (lower 32 bits)
    uint32_t timestamp32 = (uint32_t)(packet.timestamp & 0xFFFFFFFF);
    hash ^= timestamp32;
    hash *= 0x01000193;
    
    // Hash the payload (first 32 bytes max for performance)
    if (packet.payload && packet.payloadLength > 0) {
        size_t hashBytes = (packet.payloadLength < 32) ? packet.payloadLength : 32;
        for (size_t i = 0; i < hashBytes; i++) {
            hash ^= packet.payload[i];
            hash *= 0x01000193;
        }
    }
    
    return hash;
}

bool MessageRouter::isDuplicate(const BitchatPacket& packet) {
    uint32_t messageId = generateMessageId(packet);
    unsigned long now = millis();
    return dedupCache.isDuplicate(messageId, now);
}

void MessageRouter::recordMessage(const BitchatPacket& packet) {
    uint32_t messageId = generateMessageId(packet);
    unsigned long now = millis();
    dedupCache.addMessage(messageId, now);
}

void MessageRouter::handleBLEMessage(const BitchatPacket& packet, uint16_t connectionHandle) {
    Serial.printf("Message Router: Processing BLE message type 0x%02X\n", packet.type);
    
    // Check for duplicates
    if (isDuplicate(packet)) {
        Serial.printf("Message Router: Dropping duplicate BLE message (ID: 0x%08X)\n", 
                     generateMessageId(packet));
        return;
    }
    
    // Record this message to prevent future duplicates
    recordMessage(packet);
    
    // TODO: Forward to LoRa mesh for relay
    Serial.printf("Message Router: BLE message accepted for LoRa relay (TTL: %d)\n", packet.ttl);
}

void MessageRouter::handleLoRaMessage(const BitchatPacket& packet) {
    Serial.printf("Message Router: Processing LoRa message type 0x%02X\n", packet.type);
    
    // Check for duplicates
    if (isDuplicate(packet)) {
        Serial.printf("Message Router: Dropping duplicate LoRa message (ID: 0x%08X)\n", 
                     generateMessageId(packet));
        return;
    }
    
    // Record this message to prevent future duplicates
    recordMessage(packet);
    
    // TODO: Forward to connected BLE devices
    Serial.printf("Message Router: LoRa message accepted for BLE relay (TTL: %d)\n", packet.ttl);
}

void MessageRouter::performCleanup() {
    Serial.println("Message Router: Performing periodic cleanup...");
    dedupCache.cleanupExpired();
    
    // Log cache statistics
    Serial.printf("Message Router: Cache statistics - Size: %d/1000 messages\n", 
                 dedupCache.getSize());
}