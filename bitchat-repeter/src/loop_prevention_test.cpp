#include "loop_prevention_test.h"
#include "message_router.h"
#include "ble_mesh.h"
#include "lora_bridge.h"
#include <Arduino.h>

// Static member definitions
bool LoopPreventionTest::testActive = false;
unsigned long LoopPreventionTest::testStartTime = 0;
unsigned long LoopPreventionTest::testDuration = LoopPreventionTest::DEFAULT_TEST_DURATION_MS;
unsigned long LoopPreventionTest::testInterval = LoopPreventionTest::DEFAULT_TEST_INTERVAL_MS;
unsigned long LoopPreventionTest::lastTestMessage = 0;
uint8_t LoopPreventionTest::testTTL = LoopPreventionTest::DEFAULT_TEST_TTL;
uint8_t LoopPreventionTest::expectedRepeaterCount = 3;
uint32_t LoopPreventionTest::nextTestMessageId = 1000;
std::map<uint32_t, TestMessage> LoopPreventionTest::sentMessages;
TestStatistics LoopPreventionTest::statistics;
const char* LoopPreventionTest::TEST_MESSAGE_PREFIX = "LOOP_TEST_";

void LoopPreventionTest::init() {
    Serial.println("Loop Prevention Test: Initialized");
    nextTestMessageId = 1000 + random(1000); // Random starting point to avoid collisions
}

void LoopPreventionTest::startTest() {
    if (testActive) {
        Serial.println("Loop Prevention Test: Test already active");
        return;
    }
    
    testActive = true;
    testStartTime = millis();
    lastTestMessage = 0;
    sentMessages.clear();
    statistics = TestStatistics();
    
    Serial.println("===============================================");
    Serial.println("LOOP PREVENTION TEST STARTED");
    Serial.println("===============================================");
    Serial.printf("Test Duration: %.1f minutes\n", testDuration / 60000.0);
    Serial.printf("Test Interval: %.1f seconds\n", testInterval / 1000.0);
    Serial.printf("Test TTL: %d\n", testTTL);
    Serial.printf("Expected Repeaters: %d\n", expectedRepeaterCount);
    Serial.println("===============================================");
}

void LoopPreventionTest::stopTest() {
    if (!testActive) {
        Serial.println("Loop Prevention Test: No active test to stop");
        return;
    }
    
    testActive = false;
    statistics.testDuration = millis() - testStartTime;
    updateStatistics();
    
    Serial.println("===============================================");
    Serial.println("LOOP PREVENTION TEST COMPLETED");
    Serial.println("===============================================");
    printTestResults();
}

bool LoopPreventionTest::isTestActive() {
    return testActive;
}

void LoopPreventionTest::setTestInterval(unsigned long intervalMs) {
    testInterval = intervalMs;
    Serial.printf("Loop Prevention Test: Interval set to %lu ms\n", intervalMs);
}

void LoopPreventionTest::setTestDuration(unsigned long durationMs) {
    testDuration = durationMs;
    Serial.printf("Loop Prevention Test: Duration set to %lu ms\n", durationMs);
}

void LoopPreventionTest::setTestTTL(uint8_t ttl) {
    testTTL = ttl;
    Serial.printf("Loop Prevention Test: TTL set to %d\n", ttl);
}

void LoopPreventionTest::setExpectedTopology(uint8_t repeaterCount) {
    expectedRepeaterCount = repeaterCount;
    Serial.printf("Loop Prevention Test: Expected topology set to %d repeaters\n", repeaterCount);
}

void LoopPreventionTest::process() {
    if (!testActive) return;
    
    unsigned long now = millis();
    
    // Check if test duration exceeded
    if (now - testStartTime >= testDuration) {
        stopTest();
        return;
    }
    
    // Send test message at specified interval
    if (now - lastTestMessage >= testInterval) {
        sendTestMessage();
        lastTestMessage = now;
    }
    
    // Cleanup expired messages every 30 seconds
    static unsigned long lastCleanup = 0;
    if (now - lastCleanup >= 30000) {
        cleanupExpiredMessages();
        lastCleanup = now;
    }
}

void LoopPreventionTest::sendTestMessage() {
    uint32_t messageId = generateTestMessageId();
    BitchatPacket testPacket = createTestPacket(messageId, testTTL);
    
    // Track the sent message
    TestMessage testMsg(messageId, testTTL);
    sentMessages[messageId] = testMsg;
    
    // Send via message router
    MessageRouter::queueForLoRa(testPacket);
    
    statistics.totalMessagesSent++;
    
    Serial.printf("Loop Test: Sent message ID %lu with TTL %d\n", messageId, testTTL);
    
    // Free allocated payload
    if (testPacket.payload) {
        free(testPacket.payload);
    }
}

void LoopPreventionTest::handleReceivedTestMessage(const BitchatPacket& packet, const String& source) {
    if (!testActive || !isTestMessage(packet)) return;
    
    // Extract message ID from payload
    if (packet.payloadLength < strlen(TEST_MESSAGE_PREFIX) + 4) return;
    
    String payloadStr = "";
    for (uint16_t i = 0; i < packet.payloadLength; i++) {
        payloadStr += (char)packet.payload[i];
    }
    
    // Parse message ID from payload
    int prefixLen = strlen(TEST_MESSAGE_PREFIX);
    if (!payloadStr.startsWith(TEST_MESSAGE_PREFIX)) return;
    
    uint32_t messageId = payloadStr.substring(prefixLen).toInt();
    
    statistics.totalMessagesReceived++;
    
    // Check if this is our own message (shouldn't happen with proper deduplication)
    auto it = sentMessages.find(messageId);
    if (it != sentMessages.end()) {
        TestMessage& testMsg = it->second;
        testMsg.actualReceiveCount++;
        testMsg.minTtlReceived = min(testMsg.minTtlReceived, packet.ttl);
        testMsg.receivedFrom.push_back(source);
        
        if (testMsg.actualReceiveCount > testMsg.expectedReceiveCount) {
            testMsg.loopDetected = true;
            statistics.loopsDetected++;
            statistics.duplicatesDetected++;
            
            Serial.printf("LOOP DETECTED: Message ID %lu received %d times (expected %d)\n",
                         messageId, testMsg.actualReceiveCount, testMsg.expectedReceiveCount);
            Serial.printf("  Sources: ");
            for (const String& src : testMsg.receivedFrom) {
                Serial.printf("%s ", src.c_str());
            }
            Serial.println();
        } else {
            Serial.printf("Loop Test: Received own message ID %lu from %s (TTL: %d)\n",
                         messageId, source.c_str(), packet.ttl);
        }
        
        analyzeMessage(testMsg);
    } else {
        // Message from another repeater
        Serial.printf("Loop Test: Received foreign test message ID %lu from %s (TTL: %d)\n",
                     messageId, source.c_str(), packet.ttl);
    }
}

TestStatistics LoopPreventionTest::getTestStatistics() {
    if (testActive) {
        statistics.testDuration = millis() - testStartTime;
        updateStatistics();
    }
    return statistics;
}

void LoopPreventionTest::printTestResults() {
    Serial.println();
    Serial.println("LOOP PREVENTION TEST RESULTS");
    Serial.println("=============================================");
    Serial.printf("Test Duration:        %.1f minutes\n", statistics.testDuration / 60000.0);
    Serial.printf("Messages Sent:        %lu\n", statistics.totalMessagesSent);
    Serial.printf("Messages Received:    %lu\n", statistics.totalMessagesReceived);
    Serial.printf("Duplicates Detected:  %lu\n", statistics.duplicatesDetected);
    Serial.printf("Loops Detected:       %lu\n", statistics.loopsDetected);
    Serial.printf("TTL Expired:          %lu\n", statistics.ttlExpiredMessages);
    Serial.printf("Dedup Hit Rate:       %.1f%%\n", statistics.deduplicationHitRate);
    Serial.printf("Average Hop Count:    %.1f\n", statistics.averageHopCount);
    Serial.println("=============================================");
    
    if (statistics.loopsDetected == 0) {
        Serial.println("✓ PASS: No message loops detected!");
    } else {
        Serial.printf("✗ FAIL: %lu message loops detected!\n", statistics.loopsDetected);
    }
    
    if (statistics.duplicatesDetected == 0) {
        Serial.println("✓ PASS: No duplicate messages detected!");
    } else {
        Serial.printf("! WARNING: %lu duplicate messages detected\n", statistics.duplicatesDetected);
    }
    
    Serial.println("=============================================");
}

void LoopPreventionTest::printDetailedAnalysis() {
    Serial.println();
    Serial.println("DETAILED MESSAGE ANALYSIS");
    Serial.println("==========================================================");
    Serial.printf("%-10s %-5s %-5s %-8s %-8s %-6s\n", "Msg ID", "Sent", "Recv", "Min TTL", "Loop", "Status");
    Serial.println("----------------------------------------------------------");
    
    for (const auto& pair : sentMessages) {
        const TestMessage& msg = pair.second;
        String status = "OK";
        if (msg.loopDetected) status = "LOOP";
        else if (msg.actualReceiveCount == 0) status = "LOST";
        else if (msg.actualReceiveCount > 1) status = "DUP";
        
        Serial.printf("%-10lu %-5d %-5d %-8d %-8s %-6s\n",
                     msg.messageId, msg.ttlSent, msg.actualReceiveCount,
                     msg.minTtlReceived == 255 ? 0 : msg.minTtlReceived,
                     msg.loopDetected ? "YES" : "NO", status.c_str());
    }
    Serial.println("==========================================================");
}

bool LoopPreventionTest::isLoopPreventionWorking() {
    return statistics.loopsDetected == 0 && statistics.duplicatesDetected == 0;
}

void LoopPreventionTest::startTestCommand(uint8_t ttl, unsigned long durationSeconds) {
    setTestTTL(ttl);
    setTestDuration(durationSeconds * 1000);
    startTest();
}

void LoopPreventionTest::stopTestCommand() {
    stopTest();
}

void LoopPreventionTest::showTestStatus() {
    if (!testActive) {
        Serial.println("No loop prevention test is currently active");
        return;
    }
    
    unsigned long elapsed = millis() - testStartTime;
    unsigned long remaining = testDuration > elapsed ? testDuration - elapsed : 0;
    
    Serial.println("LOOP PREVENTION TEST STATUS");
    Serial.println("===============================================");
    Serial.printf("Status:           ACTIVE\n");
    Serial.printf("Elapsed:          %.1f minutes\n", elapsed / 60000.0);
    Serial.printf("Remaining:        %.1f minutes\n", remaining / 60000.0);
    Serial.printf("Messages Sent:    %lu\n", statistics.totalMessagesSent);
    Serial.printf("Messages Recv:    %lu\n", statistics.totalMessagesReceived);
    Serial.printf("Loops Detected:   %lu\n", statistics.loopsDetected);
    Serial.printf("Duplicates:       %lu\n", statistics.duplicatesDetected);
    Serial.println("===============================================");
}

// Private helper functions

BitchatPacket LoopPreventionTest::createTestPacket(uint32_t messageId, uint8_t ttl) {
    BitchatPacket packet;
    packet.type = MSG_TYPE_MESSAGE; // Regular message type
    packet.ttl = ttl;
    packet.timestamp = millis();
    
    // Set sender ID to our peer ID
    String peerID = BLEMesh::getPeerID();
    memset(packet.senderID, 0, 8);
    for (int i = 0; i < min(8, (int)peerID.length() / 2); i++) {
        String byteStr = peerID.substring(i * 2, i * 2 + 2);
        packet.senderID[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
    }
    
    // Set as broadcast
    memset(packet.recipientID, 0, 8);
    
    // Create test payload
    String payload = String(TEST_MESSAGE_PREFIX) + String(messageId);
    packet.payloadLength = payload.length();
    packet.payload = (uint8_t*)malloc(packet.payloadLength);
    memcpy(packet.payload, payload.c_str(), packet.payloadLength);
    
    return packet;
}

uint32_t LoopPreventionTest::generateTestMessageId() {
    return nextTestMessageId++;
}

bool LoopPreventionTest::isTestMessage(const BitchatPacket& packet) {
    if (packet.payloadLength < strlen(TEST_MESSAGE_PREFIX)) return false;
    
    String payloadStr = "";
    for (uint16_t i = 0; i < min((uint16_t)strlen(TEST_MESSAGE_PREFIX), packet.payloadLength); i++) {
        payloadStr += (char)packet.payload[i];
    }
    
    return payloadStr.startsWith(TEST_MESSAGE_PREFIX);
}

void LoopPreventionTest::analyzeMessage(TestMessage& testMsg) {
    // Calculate hop count (original TTL - received TTL)
    if (testMsg.minTtlReceived < 255) {
        uint8_t hopCount = testMsg.ttlSent - testMsg.minTtlReceived;
        // Update average hop count
        static uint32_t totalHops = 0;
        static uint32_t hopCountSamples = 0;
        totalHops += hopCount;
        hopCountSamples++;
        statistics.averageHopCount = (float)totalHops / hopCountSamples;
    }
}

void LoopPreventionTest::updateStatistics() {
    if (statistics.totalMessagesSent > 0) {
        statistics.deduplicationHitRate = 
            (float)statistics.duplicatesDetected / statistics.totalMessagesSent * 100.0;
    }
    
    // Count TTL expired messages (messages we sent but never received back)
    statistics.ttlExpiredMessages = 0;
    for (const auto& pair : sentMessages) {
        const TestMessage& msg = pair.second;
        if (msg.actualReceiveCount == 0 && 
            (millis() - msg.sentTime) > MESSAGE_TIMEOUT_MS) {
            statistics.ttlExpiredMessages++;
        }
    }
}

void LoopPreventionTest::cleanupExpiredMessages() {
    unsigned long now = millis();
    auto it = sentMessages.begin();
    
    while (it != sentMessages.end()) {
        if ((now - it->second.sentTime) > MESSAGE_TIMEOUT_MS) {
            it = sentMessages.erase(it);
        } else {
            ++it;
        }
    }
}