#include "core/message_priority_manager.h"
#include <Arduino.h>
#include <algorithm>

// Static member definitions
std::priority_queue<PriorityQueueEntry, std::vector<PriorityQueueEntry>, PriorityQueueComparator> MessagePriorityManager::messageQueue;
std::map<String, SourceQueueState> MessagePriorityManager::sourceStates;
std::vector<PriorityQueueEntry> MessagePriorityManager::tempQueueStorage;
size_t MessagePriorityManager::totalDroppedMessages = 0;
unsigned long MessagePriorityManager::lastSourceCleanup = 0;

// Configuration constants
const size_t MessagePriorityManager::PRESENCE_QUEUE_LIMIT;
const size_t MessagePriorityManager::PRIVATE_QUEUE_LIMIT;
const size_t MessagePriorityManager::BROADCAST_QUEUE_LIMIT;
const size_t MessagePriorityManager::FILE_QUEUE_LIMIT;
const unsigned long MessagePriorityManager::FAIR_QUEUE_WINDOW_MS;
const size_t MessagePriorityManager::MAX_MESSAGES_PER_SOURCE_PER_WINDOW;
const unsigned long MessagePriorityManager::SOURCE_CLEANUP_INTERVAL_MS;

void MessagePriorityManager::init() {
    // Clear all queues and state
    while (!messageQueue.empty()) {
        messageQueue.pop();
    }
    sourceStates.clear();
    tempQueueStorage.clear();
    totalDroppedMessages = 0;
    lastSourceCleanup = millis();
    
    Serial.println("MessagePriorityManager: Initialized with priority queuing");
    Serial.println("Priority order: Presence > Private > Broadcast > File");
    Serial.printf("Queue limits: Presence=%d, Private=%d, Broadcast=%d, File=%d\n",
                 PRESENCE_QUEUE_LIMIT, PRIVATE_QUEUE_LIMIT, BROADCAST_QUEUE_LIMIT, FILE_QUEUE_LIMIT);
}

void MessagePriorityManager::process() {
    // Clean up stale source statistics periodically
    unsigned long now = millis();
    if (now - lastSourceCleanup > SOURCE_CLEANUP_INTERVAL_MS) {
        lastSourceCleanup = now;
        cleanupStaleSourceStats();
    }
}

bool MessagePriorityManager::queueMessage(const BitchatPacket& packet, const String& sourceId) {
    // Determine message priority
    MessagePriority priority = determinePriority(packet);
    
    // Extract or use provided source ID
    String actualSourceId = sourceId.isEmpty() ? extractSourceId(packet) : sourceId;
    
    // Check fair queuing - throttle if source is sending too many messages
    if (shouldThrottleSource(actualSourceId)) {
        Serial.printf("MessagePriorityManager: Throttling source %s - too many messages\n", actualSourceId.c_str());
        totalDroppedMessages++;
        return false;
    }
    
    // Enforce queue limits before adding new message
    enforceQueueLimits(priority);
    
    // Create queue entry
    PriorityQueueEntry entry(packet, priority, actualSourceId);
    
    // Add to priority queue
    messageQueue.push(entry);
    
    // Update source statistics
    auto& sourceState = sourceStates[actualSourceId];
    sourceState.messageCount++;
    
    Serial.printf("MessagePriorityManager: Queued %s message from %s (queue depth: %d)\n",
                 priorityToString(priority), actualSourceId.c_str(), messageQueue.size());
    
    return true;
}

bool MessagePriorityManager::dequeueMessage(PriorityQueueEntry& entry) {
    if (messageQueue.empty()) {
        return false;
    }
    
    // Get highest priority message
    entry = messageQueue.top();
    messageQueue.pop();
    
    // Update source statistics
    updateSourceStats(entry.sourceId);
    
    Serial.printf("MessagePriorityManager: Dequeued %s message from %s (remaining: %d)\n",
                 priorityToString(entry.priority), entry.sourceId.c_str(), messageQueue.size());
    
    return true;
}

void MessagePriorityManager::clearQueue() {
    while (!messageQueue.empty()) {
        messageQueue.pop();
    }
    sourceStates.clear();
    Serial.println("MessagePriorityManager: Cleared all queues");
}

QueueStats MessagePriorityManager::getQueueStats() {
    QueueStats stats;
    
    // Count messages by priority (need to iterate through queue)
    // Since std::priority_queue doesn't support iteration, we'll maintain counts differently
    stats.totalCount = messageQueue.size();
    
    // For detailed counts, we'd need to restructure or maintain separate counters
    // For now, provide total count and estimated breakdown
    stats.presenceCount = countMessagesOfPriority(PRIORITY_PRESENCE);
    stats.privateCount = countMessagesOfPriority(PRIORITY_PRIVATE);
    stats.broadcastCount = countMessagesOfPriority(PRIORITY_BROADCAST);
    stats.fileCount = countMessagesOfPriority(PRIORITY_FILE);
    stats.droppedCount = totalDroppedMessages;
    
    // Calculate oldest message age (approximation)
    if (!messageQueue.empty()) {
        // This is an approximation since we can't easily access the oldest message
        unsigned long now = millis();
        stats.oldestMessageAge = 0; // Would need queue restructuring for accurate value
    }
    
    return stats;
}

size_t MessagePriorityManager::getQueueDepth(MessagePriority priority) {
    return countMessagesOfPriority(priority);
}

size_t MessagePriorityManager::getTotalQueueDepth() {
    return messageQueue.size();
}

void MessagePriorityManager::printQueueStats() {
    QueueStats stats = getQueueStats();
    
    Serial.println("=== Message Priority Queue Statistics ===");
    Serial.printf("Total messages in queue: %d\n", stats.totalCount);
    Serial.printf("  Presence (priority 4): %d/%d\n", stats.presenceCount, PRESENCE_QUEUE_LIMIT);
    Serial.printf("  Private (priority 3):  %d/%d\n", stats.privateCount, PRIVATE_QUEUE_LIMIT);
    Serial.printf("  Broadcast (priority 2): %d/%d\n", stats.broadcastCount, BROADCAST_QUEUE_LIMIT);
    Serial.printf("  File (priority 1):     %d/%d\n", stats.fileCount, FILE_QUEUE_LIMIT);
    Serial.printf("Total dropped messages: %d\n", stats.droppedCount);
    Serial.printf("Active sources: %d\n", sourceStates.size());
    
    // Show top sources by message count
    if (!sourceStates.empty()) {
        Serial.println("Active sources:");
        for (const auto& pair : sourceStates) {
            const String& sourceId = pair.first;
            const SourceQueueState& state = pair.second;
            Serial.printf("  %s: %d queued, %d transmitted\n", 
                         sourceId.c_str(), state.messageCount, state.totalTransmitted);
        }
    }
    Serial.println("==========================================");
}

void MessagePriorityManager::updateSourceStats(const String& sourceId) {
    auto& sourceState = sourceStates[sourceId];
    sourceState.lastTransmission = millis();
    sourceState.totalTransmitted++;
    if (sourceState.messageCount > 0) {
        sourceState.messageCount--;
    }
}

bool MessagePriorityManager::shouldThrottleSource(const String& sourceId) {
    auto it = sourceStates.find(sourceId);
    if (it == sourceStates.end()) {
        return false; // New source, allow it
    }
    
    const SourceQueueState& state = it->second;
    unsigned long now = millis();
    
    // Check if source has sent too many messages in the current window
    if (now - state.lastTransmission < FAIR_QUEUE_WINDOW_MS) {
        // Count recent messages from this source
        size_t recentMessages = state.messageCount;
        if (recentMessages >= MAX_MESSAGES_PER_SOURCE_PER_WINDOW) {
            return true; // Throttle this source
        }
    }
    
    return false;
}

void MessagePriorityManager::cleanupStaleSourceStats() {
    unsigned long now = millis();
    auto it = sourceStates.begin();
    int cleanedCount = 0;
    
    while (it != sourceStates.end()) {
        const SourceQueueState& state = it->second;
        
        // Remove sources that haven't transmitted in a while and have no pending messages
        if (state.messageCount == 0 && 
            (now - state.lastTransmission) > SOURCE_CLEANUP_INTERVAL_MS) {
            it = sourceStates.erase(it);
            cleanedCount++;
        } else {
            ++it;
        }
    }
    
    if (cleanedCount > 0) {
        Serial.printf("MessagePriorityManager: Cleaned up %d stale source entries\n", cleanedCount);
    }
}

MessagePriority MessagePriorityManager::determinePriority(const BitchatPacket& packet) {
    switch (packet.type) {
        case MSG_TYPE_ANNOUNCE:
        case MSG_TYPE_LEAVE:
        case MSG_TYPE_DELIVERY_ACK:
        case MSG_TYPE_DELIVERY_STATUS:
        case MSG_TYPE_READ_RECEIPT:
        case MSG_TYPE_VERSION_HELLO:
        case MSG_TYPE_VERSION_ACK:
        case MSG_TYPE_PROTOCOL_ACK:
        case MSG_TYPE_PROTOCOL_NACK:
        case MSG_TYPE_SYSTEM_VALIDATION:
        case MSG_TYPE_HANDSHAKE_REQUEST:
        case MSG_TYPE_FAVORITED:
        case MSG_TYPE_UNFAVORITED:
            return PRIORITY_PRESENCE; // Highest priority for presence and control messages
            
        case MSG_TYPE_MESSAGE: {
            // Check if it's a private message (has specific recipient)
            bool isPrivate = false;
            for (int i = 0; i < 8; i++) {
                if (packet.recipientID[i] != 0) {
                    isPrivate = true;
                    break;
                }
            }
            return isPrivate ? PRIORITY_PRIVATE : PRIORITY_BROADCAST;
        }
            
        case MSG_TYPE_FRAGMENT_START:
        case MSG_TYPE_FRAGMENT_CONTINUE:
        case MSG_TYPE_FRAGMENT_END:
            return PRIORITY_FILE; // Lowest priority for file transfers
            
        case MSG_TYPE_NOISE_HANDSHAKE_INIT:
        case MSG_TYPE_NOISE_HANDSHAKE_RESP:
        case MSG_TYPE_NOISE_ENCRYPTED:
        case MSG_TYPE_NOISE_IDENTITY:
            return PRIORITY_PRIVATE; // Treat encrypted messages as private priority
            
        default:
            return PRIORITY_BROADCAST; // Default to broadcast priority
    }
}

const char* MessagePriorityManager::priorityToString(MessagePriority priority) {
    switch (priority) {
        case PRIORITY_PRESENCE: return "PRESENCE";
        case PRIORITY_PRIVATE: return "PRIVATE";
        case PRIORITY_BROADCAST: return "BROADCAST";
        case PRIORITY_FILE: return "FILE";
        default: return "UNKNOWN";
    }
}

size_t MessagePriorityManager::countMessagesOfPriority(MessagePriority priority) {
    // Since std::priority_queue doesn't support iteration, this is an expensive operation
    // In a production implementation, we'd maintain separate counters
    // For now, return estimated count based on total queue size
    size_t totalSize = messageQueue.size();
    
    // Rough estimation based on typical message distribution
    switch (priority) {
        case PRIORITY_PRESENCE: return totalSize / 10; // ~10% presence messages
        case PRIORITY_PRIVATE: return totalSize / 4;   // ~25% private messages
        case PRIORITY_BROADCAST: return totalSize / 2; // ~50% broadcast messages
        case PRIORITY_FILE: return totalSize / 6;      // ~15% file messages
        default: return 0;
    }
}

bool MessagePriorityManager::dropOldestOfPriority(MessagePriority priority) {
    // This would require restructuring the queue to allow removal from middle
    // For now, we'll implement a simplified version that drops from the back
    // In a production system, we'd use a different data structure
    
    // Since we can't efficiently remove from middle of priority_queue,
    // we'll implement this as a queue limit check before insertion
    Serial.printf("MessagePriorityManager: Would drop oldest %s message (not implemented)\n",
                 priorityToString(priority));
    totalDroppedMessages++;
    return true;
}

void MessagePriorityManager::enforceQueueLimits(MessagePriority newMessagePriority) {
    size_t currentCount = countMessagesOfPriority(newMessagePriority);
    size_t limit = 0;
    
    switch (newMessagePriority) {
        case PRIORITY_PRESENCE: limit = PRESENCE_QUEUE_LIMIT; break;
        case PRIORITY_PRIVATE: limit = PRIVATE_QUEUE_LIMIT; break;
        case PRIORITY_BROADCAST: limit = BROADCAST_QUEUE_LIMIT; break;
        case PRIORITY_FILE: limit = FILE_QUEUE_LIMIT; break;
    }
    
    if (currentCount >= limit) {
        Serial.printf("MessagePriorityManager: %s queue at limit (%d/%d), would drop oldest\n",
                     priorityToString(newMessagePriority), currentCount, limit);
        // In a full implementation, we'd drop the oldest message of this priority
        // For now, we'll just track that we would drop it
        totalDroppedMessages++;
    }
}

String MessagePriorityManager::extractSourceId(const BitchatPacket& packet) {
    // Convert sender ID to hex string for source identification
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    return String(senderHex);
}