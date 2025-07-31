#pragma once

#include <Arduino.h>
#include "bitchat_protocol.h"
#include <map>
#include <vector>

struct TestMessage {
    uint32_t messageId;
    unsigned long sentTime;
    uint8_t expectedReceiveCount;
    uint8_t actualReceiveCount;
    uint8_t ttlSent;
    uint8_t minTtlReceived;
    bool loopDetected;
    std::vector<String> receivedFrom; // Track which sources we received duplicates from
    
    TestMessage() : messageId(0), sentTime(0), expectedReceiveCount(1), 
                   actualReceiveCount(0), ttlSent(5), minTtlReceived(255), 
                   loopDetected(false) {}
    
    TestMessage(uint32_t id, uint8_t ttl) : messageId(id), sentTime(millis()), 
                                           expectedReceiveCount(1), actualReceiveCount(0),
                                           ttlSent(ttl), minTtlReceived(255), loopDetected(false) {}
};

struct TestStatistics {
    uint32_t totalMessagesSent;
    uint32_t totalMessagesReceived;
    uint32_t duplicatesDetected;
    uint32_t loopsDetected;
    uint32_t ttlExpiredMessages;
    float deduplicationHitRate;
    float averageHopCount;
    unsigned long testDuration;
    
    TestStatistics() : totalMessagesSent(0), totalMessagesReceived(0), 
                      duplicatesDetected(0), loopsDetected(0), ttlExpiredMessages(0),
                      deduplicationHitRate(0.0), averageHopCount(0.0), testDuration(0) {}
};

class LoopPreventionTest {
public:
    static void init();
    static void startTest();
    static void stopTest();
    static bool isTestActive();
    
    // Test configuration
    static void setTestInterval(unsigned long intervalMs);
    static void setTestDuration(unsigned long durationMs);
    static void setTestTTL(uint8_t ttl);
    static void setExpectedTopology(uint8_t repeaterCount);
    
    // Test execution
    static void process();
    static void sendTestMessage();
    static void handleReceivedTestMessage(const BitchatPacket& packet, const String& source);
    
    // Results and analysis
    static TestStatistics getTestStatistics();
    static void printTestResults();
    static void printDetailedAnalysis();
    static bool isLoopPreventionWorking();
    
    // Serial command interface
    static void startTestCommand(uint8_t ttl = 5, unsigned long durationSeconds = 300);
    static void stopTestCommand();
    static void showTestStatus();
    
private:
    static bool testActive;
    static unsigned long testStartTime;
    static unsigned long testDuration;
    static unsigned long testInterval;
    static unsigned long lastTestMessage;
    static uint8_t testTTL;
    static uint8_t expectedRepeaterCount;
    static uint32_t nextTestMessageId;
    
    // Test tracking
    static std::map<uint32_t, TestMessage> sentMessages;
    static TestStatistics statistics;
    
    // Test message generation
    static BitchatPacket createTestPacket(uint32_t messageId, uint8_t ttl);
    static uint32_t generateTestMessageId();
    static bool isTestMessage(const BitchatPacket& packet);
    
    // Analysis functions
    static void analyzeMessage(TestMessage& testMsg);
    static void updateStatistics();
    static void cleanupExpiredMessages();
    
    // Constants
    static const unsigned long DEFAULT_TEST_INTERVAL_MS = 10000; // 10 seconds
    static const unsigned long DEFAULT_TEST_DURATION_MS = 300000; // 5 minutes
    static const unsigned long MESSAGE_TIMEOUT_MS = 30000; // 30 seconds
    static const uint8_t DEFAULT_TEST_TTL = 5;
    static const char* TEST_MESSAGE_PREFIX;
};