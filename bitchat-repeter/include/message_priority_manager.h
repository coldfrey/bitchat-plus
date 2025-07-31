#pragma once

#include <queue>
#include <map>
#include <vector>
#include <Arduino.h>
#include "bitchat_protocol.h"

// Message priority levels (higher number = higher priority)
enum MessagePriority {
    PRIORITY_FILE = 1,          // Lowest priority - file transfers
    PRIORITY_BROADCAST = 2,     // Broadcast messages  
    PRIORITY_PRIVATE = 3,       // Private messages
    PRIORITY_PRESENCE = 4       // Highest priority - presence/announcements
};

// Queue entry for prioritized messages
struct PriorityQueueEntry {
    BitchatPacket packet;
    uint8_t packetData[256];        // Storage for packet payload
    unsigned long queueTime;       // When message was queued
    unsigned long scheduleTime;    // When to transmit (with random delay)
    uint8_t retryCount;           // Number of transmission attempts
    MessagePriority priority;     // Message priority level
    String sourceId;              // Source identifier for fair queuing
    
    PriorityQueueEntry() : queueTime(0), scheduleTime(0), retryCount(0), 
                          priority(PRIORITY_BROADCAST) {
        memset(packetData, 0, sizeof(packetData));
    }
    
    PriorityQueueEntry(const BitchatPacket& pkt, MessagePriority prio, const String& source) :
        packet(pkt), queueTime(millis()), scheduleTime(millis()), retryCount(0), 
        priority(prio), sourceId(source) {
        memset(packetData, 0, sizeof(packetData));
        // Copy packet payload data
        if (pkt.payload && pkt.payloadLength > 0) {
            size_t copySize = min((size_t)pkt.payloadLength, sizeof(packetData));
            memcpy(packetData, pkt.payload, copySize);
            // Update packet pointer to local storage
            const_cast<BitchatPacket&>(packet).payload = packetData;
        }
    }
};

// Comparison operator for priority queue (lower value = higher priority in std::priority_queue)
struct PriorityQueueComparator {
    bool operator()(const PriorityQueueEntry& a, const PriorityQueueEntry& b) const {
        // First compare by priority (higher priority first)
        if (a.priority != b.priority) {
            return a.priority < b.priority; // Higher priority number = higher priority
        }
        // If same priority, older messages first (FIFO within priority)
        return a.queueTime > b.queueTime;
    }
};

// Fair queuing state tracking
struct SourceQueueState {
    unsigned long lastTransmission; // Last time this source transmitted
    size_t messageCount;            // Number of messages in queue from this source
    size_t totalTransmitted;        // Total messages transmitted from this source
    
    SourceQueueState() : lastTransmission(0), messageCount(0), totalTransmitted(0) {}
};

// Queue statistics for monitoring
struct QueueStats {
    size_t presenceCount;
    size_t privateCount;
    size_t broadcastCount;
    size_t fileCount;
    size_t totalCount;
    size_t droppedCount;
    unsigned long oldestMessageAge;
    
    QueueStats() : presenceCount(0), privateCount(0), broadcastCount(0), 
                  fileCount(0), totalCount(0), droppedCount(0), oldestMessageAge(0) {}
};

class MessagePriorityManager {
public:
    static void init();
    static void process();
    
    // Queue management
    static bool queueMessage(const BitchatPacket& packet, const String& sourceId = "");
    static bool dequeueMessage(PriorityQueueEntry& entry);
    static void clearQueue();
    
    // Queue monitoring
    static QueueStats getQueueStats();
    static size_t getQueueDepth(MessagePriority priority);
    static size_t getTotalQueueDepth();
    static void printQueueStats();
    
    // Fair queuing management
    static void updateSourceStats(const String& sourceId);
    static bool shouldThrottleSource(const String& sourceId);
    static void cleanupStaleSourceStats();
    
    // Priority determination
    static MessagePriority determinePriority(const BitchatPacket& packet);
    static const char* priorityToString(MessagePriority priority);
    static String extractSourceId(const BitchatPacket& packet);
    
private:
    // Priority queues using std::priority_queue with custom comparator
    static std::priority_queue<PriorityQueueEntry, std::vector<PriorityQueueEntry>, PriorityQueueComparator> messageQueue;
    
    // Queue limits per priority level
    static const size_t PRESENCE_QUEUE_LIMIT = 10;
    static const size_t PRIVATE_QUEUE_LIMIT = 20;
    static const size_t BROADCAST_QUEUE_LIMIT = 30;
    static const size_t FILE_QUEUE_LIMIT = 5;
    
    // Fair queuing management
    static std::map<String, SourceQueueState> sourceStates;
    static const unsigned long FAIR_QUEUE_WINDOW_MS = 1000;    // 1 second window
    static const size_t MAX_MESSAGES_PER_SOURCE_PER_WINDOW = 5; // Max 5 messages per source per second
    static const unsigned long SOURCE_CLEANUP_INTERVAL_MS = 300000; // 5 minutes
    
    // Statistics tracking
    static size_t totalDroppedMessages;
    static unsigned long lastSourceCleanup;
    
    // Queue management helpers
    static size_t countMessagesOfPriority(MessagePriority priority);
    static bool dropOldestOfPriority(MessagePriority priority);
    static void enforceQueueLimits(MessagePriority newMessagePriority);
    
    // Temporary storage for queue iteration (since std::priority_queue doesn't support iteration)
    static std::vector<PriorityQueueEntry> tempQueueStorage;
};