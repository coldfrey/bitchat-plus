#include "message_router.h"
#include "lora_bridge.h"
#include "ble_mesh.h"
#include <Arduino.h>

// Static member definitions
MessageCache MessageRouter::dedupCache;
unsigned long MessageRouter::lastCleanup = 0;
std::vector<LoRaQueueEntry> MessageRouter::loraQueue;

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
    unsigned long now = millis();
    
    // Process LoRa transmission queue
    processLoRaQueue();
    
    // Perform periodic cleanup every 30 seconds
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
    Serial.printf("Message Router: Processing BLE message type 0x%02X from connection %d\n", 
                 packet.type, connectionHandle);
    
    // Check for duplicates
    if (isDuplicate(packet)) {
        Serial.printf("Message Router: Dropping duplicate BLE message (ID: 0x%08X)\n", 
                     generateMessageId(packet));
        return;
    }
    
    // Create a mutable copy for TTL processing
    BitchatPacket routingPacket = packet;
    
    // Decrement TTL and check if we should forward
    if (!decrementTTL(routingPacket)) {
        Serial.printf("Message Router: Dropping BLE message - TTL reached 0\n");
        return;
    }
    
    // Check if we should forward this message
    if (!shouldForward(routingPacket, connectionHandle)) {
        Serial.printf("Message Router: Not forwarding BLE message - routing decision\n");
        return;
    }
    
    // Record this message to prevent future duplicates
    recordMessage(packet); // Record original packet to maintain consistency
    
    // Queue for LoRa mesh transmission
    if (queueForLoRa(routingPacket)) {
        Serial.printf("Message Router: BLE message queued for LoRa relay (TTL: %d)\n", routingPacket.ttl);
    } else {
        Serial.printf("Message Router: Failed to queue BLE message for LoRa (queue full)\n");
    }
}

void MessageRouter::handleLoRaMessage(const BitchatPacket& packet) {
    Serial.printf("Message Router: Processing LoRa message type 0x%02X\n", packet.type);
    
    // Check for duplicates
    if (isDuplicate(packet)) {
        Serial.printf("Message Router: Dropping duplicate LoRa message (ID: 0x%08X)\n", 
                     generateMessageId(packet));
        return;
    }
    
    // Create a mutable copy for TTL processing  
    BitchatPacket routingPacket = packet;
    
    // Decrement TTL and check if we should forward
    if (!decrementTTL(routingPacket)) {
        Serial.printf("Message Router: Dropping LoRa message - TTL reached 0\n");
        return;
    }
    
    // Check if we should forward this message
    if (!shouldForward(routingPacket)) {
        Serial.printf("Message Router: Not forwarding LoRa message - routing decision\n");
        return;
    }
    
    // Record this message to prevent future duplicates
    recordMessage(packet); // Record original packet to maintain consistency
    
    // Forward to all connected BLE devices (iOS apps)
    forwardToBLE(routingPacket);
    
    // Also queue for further LoRa relay if TTL allows
    if (routingPacket.ttl > 1) { // Only relay if there's still hop count remaining
        if (queueForLoRa(routingPacket)) {
            Serial.printf("Message Router: LoRa message also queued for further LoRa relay (TTL: %d)\n", 
                         routingPacket.ttl);
        }
    }
    
    Serial.printf("Message Router: LoRa message processed for BLE relay (TTL: %d)\n", routingPacket.ttl);
}

void MessageRouter::performCleanup() {
    Serial.println("Message Router: Performing periodic cleanup...");
    dedupCache.cleanupExpired();
    
    // Log cache and queue statistics
    Serial.printf("Message Router: Cache: %d/1000 messages, LoRa Queue: %d/%d\n", 
                 dedupCache.getSize(), loraQueue.size(), MAX_LORA_QUEUE_SIZE);
}

// TTL and routing logic implementation
bool MessageRouter::decrementTTL(BitchatPacket& packet) {
    if (packet.ttl <= 1) {
        return false; // TTL exhausted, don't forward
    }
    packet.ttl--; // Decrement for next hop
    return true;
}

bool MessageRouter::shouldForward(const BitchatPacket& packet, uint16_t sourceConnectionHandle) {
    // Basic forwarding logic - can be enhanced later
    
    // Don't forward version negotiation messages (they're direct only)
    if (packet.type == MSG_TYPE_VERSION_HELLO || packet.type == MSG_TYPE_VERSION_ACK) {
        return false;
    }
    
    // Don't forward messages with TTL of 0 or 1 (no hops left)
    if (packet.ttl <= 1) {
        return false;
    }
    
    // Forward all other supported message types
    switch (packet.type) {
        case MSG_TYPE_ANNOUNCE:
        case MSG_TYPE_LEAVE:
        case MSG_TYPE_MESSAGE:
        case MSG_TYPE_DELIVERY_ACK:
        case MSG_TYPE_PROTOCOL_ACK:
            return true;
        default:
            return false; // Unknown message types not forwarded
    }
}

// LoRa transmission queue implementation
bool MessageRouter::queueForLoRa(const BitchatPacket& packet) {
    // Check queue capacity
    if (loraQueue.size() >= MAX_LORA_QUEUE_SIZE) {
        Serial.printf("Message Router: LoRa queue full (%d/%d), dropping message\n", 
                     loraQueue.size(), MAX_LORA_QUEUE_SIZE);
        return false;
    }
    
    // Create queue entry
    LoRaQueueEntry entry;
    entry.packet = packet;
    entry.scheduleTime = millis() + getRandomDelay(); // Add random delay
    entry.retryCount = 0;
    
    // Copy packet payload data to our storage
    copyPacketData(entry, packet);
    
    // Add to queue
    loraQueue.push_back(entry);
    
    Serial.printf("Message Router: Queued message for LoRa transmission (delay: %dms, queue: %d/%d)\n",
                 (int)(entry.scheduleTime - millis()), loraQueue.size(), MAX_LORA_QUEUE_SIZE);
    
    return true;
}

void MessageRouter::processLoRaQueue() {
    if (loraQueue.empty()) {
        return;
    }
    
    unsigned long now = millis();
    
    // Process entries that are ready to transmit
    for (auto it = loraQueue.begin(); it != loraQueue.end(); ) {
        if (now >= it->scheduleTime) {
            Serial.printf("Message Router: Transmitting queued LoRa message (type: 0x%02X, TTL: %d)\n",
                         it->packet.type, it->packet.ttl);
            
            // Transmit via LoRa bridge
            bool success = LoRaBridge::transmit(it->packet);
            
            if (success) {
                // Remove from queue after successful transmission
                it = loraQueue.erase(it);
            } else {
                // Increment retry count and reschedule
                it->retryCount++;
                if (it->retryCount >= 3) {
                    Serial.printf("Message Router: Dropping message after %d failed attempts\n", it->retryCount);
                    it = loraQueue.erase(it);
                } else {
                    // Retry with exponential backoff
                    it->scheduleTime = now + (100 << it->retryCount); // 100ms, 200ms, 400ms
                    Serial.printf("Message Router: Rescheduling transmission (attempt %d) in %dms\n", 
                                 it->retryCount + 1, (100 << it->retryCount));
                    ++it;
                }
            }
        } else {
            ++it;
        }
    }
}

void MessageRouter::forwardToBLE(const BitchatPacket& packet) {
    Serial.printf("Message Router: Forwarding LoRa message to BLE devices (type: 0x%02X)\n", packet.type);
    
    // Serialize packet for BLE transmission
    uint8_t buffer[256];
    size_t packetSize = serializePacket(packet, buffer, sizeof(buffer));
    
    if (packetSize == 0) {
        Serial.println("Message Router: Failed to serialize packet for BLE forwarding");
        return;
    }
    
    // Send to all connected BLE devices
    if (BLEMesh::getConnectedClientCount() > 0) {
        BLEMesh::sendData(buffer, packetSize);
        Serial.printf("Message Router: Forwarded LoRa message to %d connected BLE device(s)\n", 
                     BLEMesh::getConnectedClientCount());
    } else {
        Serial.println("Message Router: No BLE devices connected - message not forwarded");
    }
}

// Helper functions
unsigned long MessageRouter::getRandomDelay() {
    // Random delay between 0-100ms to prevent LoRa collisions
    return random(0, 101);
}

void MessageRouter::copyPacketData(LoRaQueueEntry& entry, const BitchatPacket& packet) {
    // Copy payload data to our local storage
    if (packet.payload && packet.payloadLength > 0) {
        size_t copySize = (packet.payloadLength < sizeof(entry.packetData)) ? 
                         packet.payloadLength : sizeof(entry.packetData);
        memcpy(entry.packetData, packet.payload, copySize);
        entry.packet.payload = entry.packetData; // Point to our storage
    } else {
        entry.packet.payload = nullptr;
    }
}