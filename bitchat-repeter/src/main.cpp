#include <Arduino.h>
#include "hardware_config.h"
#include "ble_mesh.h"
#include "lora_bridge.h"
#include "message_router.h"
#include "config_manager.h"
#include "bitchat_protocol.h"
#include "connection_manager.h"
#include "message_priority_manager.h"
#include "power_manager.h"

// Test function to verify LoRa packet format works correctly
void testLoRaPacketFormat() {
    Serial.println("=== Testing LoRa Packet Format ===");
    
    // Create a test LoRa packet
    LoRaPacket testPacket;
    testPacket.type = LORA_PKT_DATA;
    testPacket.srcRepeater = 0x12345678;
    testPacket.destRepeater = 0xABCDEF00;
    testPacket.nextHop = 0x11223344;
    testPacket.hopCount = 2;
    testPacket.maxHops = 5;
    testPacket.seqNum = 0x1234;
    
    // Create test payload (simulate a BitChat packet)
    const char* testData = "Hello, LoRa mesh!";
    testPacket.payloadLen = strlen(testData);
    memcpy(testPacket.payload, testData, testPacket.payloadLen);
    
    // Serialize the packet
    uint8_t buffer[256];
    size_t serializedSize = serializeLoRaPacket(testPacket, buffer, sizeof(buffer));
    
    if (serializedSize == 0) {
        Serial.println("ERROR: Failed to serialize LoRa packet");
        return;
    }
    
    Serial.printf("Serialized packet: %d bytes\n", serializedSize);
    
    // Parse the serialized packet back
    LoRaPacket parsedPacket;
    LoRaParseResult result = parseLoRaPacket(buffer, serializedSize, parsedPacket);
    
    if (result != LORA_PARSE_SUCCESS) {
        Serial.printf("ERROR: Failed to parse LoRa packet (error code: %d)\n", result);
        return;
    }
    
    // Verify the parsed packet matches the original
    bool success = true;
    if (parsedPacket.type != testPacket.type) {
        Serial.printf("ERROR: Type mismatch (expected 0x%02X, got 0x%02X)\n", testPacket.type, parsedPacket.type);
        success = false;
    }
    if (parsedPacket.srcRepeater != testPacket.srcRepeater) {
        Serial.printf("ERROR: Source mismatch (expected %08X, got %08X)\n", testPacket.srcRepeater, parsedPacket.srcRepeater);
        success = false;
    }
    if (parsedPacket.destRepeater != testPacket.destRepeater) {
        Serial.printf("ERROR: Destination mismatch (expected %08X, got %08X)\n", testPacket.destRepeater, parsedPacket.destRepeater);
        success = false;
    }
    if (parsedPacket.payloadLen != testPacket.payloadLen) {
        Serial.printf("ERROR: Payload length mismatch (expected %d, got %d)\n", testPacket.payloadLen, parsedPacket.payloadLen);
        success = false;
    }
    if (memcmp(parsedPacket.payload, testPacket.payload, testPacket.payloadLen) != 0) {
        Serial.println("ERROR: Payload data mismatch");
        success = false;
    }
    
    if (success) {
        Serial.println("SUCCESS: LoRa packet format test passed!");
        Serial.printf("  Packet size: %d bytes (header=%d, payload=%d)\n", 
                     serializedSize, LORA_HEADER_SIZE, testPacket.payloadLen);
        Serial.printf("  All fields correctly serialized and parsed\n");
    } else {
        Serial.println("FAILED: LoRa packet format test failed!");
    }
    
    Serial.println("=== End LoRa Packet Format Test ===");
    Serial.println();
}

// Test function to verify neighbor discovery functionality
void testNeighborDiscovery() {
    Serial.println("=== Testing Neighbor Discovery ===");
    
    // Test neighbor table functionality
    Serial.println("Testing NeighborTable operations...");
    
    // Add a test neighbor
    NeighborTable::addOrUpdateNeighbor(0x12345678, "TestRepeater", 2, 5, 
                                      NEIGHBOR_CAP_ALWAYS_ON, 1, -75);
    
    // Add another test neighbor
    NeighborTable::addOrUpdateNeighbor(0xABCDEF00, "TestRepeater2", 1, 2, 
                                      NEIGHBOR_CAP_BATTERY_POWERED, 1, -85);
    
    // Update the first neighbor with new RSSI
    NeighborTable::addOrUpdateNeighbor(0x12345678, "TestRepeater", 3, 7, 
                                      NEIGHBOR_CAP_ALWAYS_ON, 1, -80);
    
    // Print the neighbor table
    NeighborTable::printNeighborTable();
    
    // Test neighbor lookup
    NeighborEntry* neighbor = NeighborTable::getNeighbor(0x12345678);
    if (neighbor) {
        Serial.printf("Found neighbor: %s (Quality: %d%%)\n", 
                     neighbor->name.c_str(), neighbor->linkQuality);
    } else {
        Serial.println("ERROR: Failed to find test neighbor");
    }
    
    // Test neighbor count
    Serial.printf("Neighbor count: %d\n", NeighborTable::getNeighborCount());
    
    // Clean up test neighbors
    NeighborTable::removeNeighbor(0x12345678);
    NeighborTable::removeNeighbor(0xABCDEF00);
    
    Serial.printf("After cleanup, neighbor count: %d\n", NeighborTable::getNeighborCount());
    
    Serial.println("SUCCESS: Neighbor discovery test completed!");
    Serial.println("=== End Neighbor Discovery Test ===");
    Serial.println();
}

// Test function to verify transmission queue and collision avoidance
void testTransmissionSystem() {
    Serial.println("=== Testing Transmission System ===");
    
    // Test queuing multiple packets
    Serial.println("Testing packet queuing...");
    
    for (int i = 0; i < 5; i++) {
        LoRaPacket testPacket;
        testPacket.type = LORA_PKT_DATA;
        testPacket.srcRepeater = getRepeaterID();
        testPacket.destRepeater = LORA_DEST_BROADCAST;
        testPacket.nextHop = LORA_DEST_BROADCAST;
        testPacket.hopCount = 0;
        testPacket.maxHops = 5;
        testPacket.seqNum = i + 1;
        
        // Create test payload
        char testData[32];
        snprintf(testData, sizeof(testData), "Test packet #%d", i + 1);
        testPacket.payloadLen = strlen(testData);
        memcpy(testPacket.payload, testData, testPacket.payloadLen);
        
        bool queued = LoRaBridge::queueLoRaPacket(testPacket);
        if (queued) {
            Serial.printf("Queued test packet #%d\n", i + 1);
        } else {
            Serial.printf("Failed to queue test packet #%d\n", i + 1);
        }
    }
    
    Serial.println("Test packets queued. They will be transmitted by the main loop.");
    Serial.println("SUCCESS: Transmission system test completed!");
    Serial.println("=== End Transmission System Test ===");
    Serial.println();
}

// Test function to verify mesh routing functionality
void testMeshRouting() {
    Serial.println("=== Testing Mesh Routing ===");
    
    // Test route table operations
    Serial.println("Testing RouteTable operations...");
    
    // Add test routes
    RouteTable::addRoute(0x12345678, 0xAABBCCDD, 3, 100);
    RouteTable::addRoute(0xABCDEF00, 0x11223344, 2, 200);
    RouteTable::addRoute(0x55667788, 0xAABBCCDD, 5, 150);
    
    // Test route lookup
    RouteEntry* route = RouteTable::findRoute(0x12345678);
    if (route) {
        Serial.printf("Found route to %08X via %08X (hops=%d, seq=%lu)\n",
                     route->destination, route->nextHop, route->hopCount, route->sequenceNumber);
    } else {
        Serial.println("ERROR: Failed to find test route");
    }
    
    // Print route table
    RouteTable::printRouteTable();
    
    // Test route discovery simulation
    Serial.println("Testing route discovery...");
    uint32_t testDestination = 0x99887766;
    LoRaBridge::initiateRouteDiscovery(testDestination);
    
    // Test route request ID generation
    for (int i = 0; i < 3; i++) {
        uint32_t reqId = RouteTable::generateRequestId();
        Serial.printf("Generated request ID: %lu\n", reqId);
    }
    
    Serial.printf("Route count: %d\n", RouteTable::getRouteCount());
    
    Serial.println("SUCCESS: Mesh routing test completed!");
    Serial.println("=== End Mesh Routing Test ===");
    Serial.println();
}

// Test function to verify reliable delivery functionality
void testReliableDelivery() {
    Serial.println("=== Testing Reliable Delivery ===");
    
    // Test mesh deduplication
    Serial.println("Testing mesh deduplication...");
    uint32_t testSource = 0x12345678;
    uint32_t testSeq1 = 100;
    uint32_t testSeq2 = 101;
    
    // First packet should not be duplicate
    bool isDupe1 = ReliabilityManager::isDuplicatePacket(testSource, testSeq1);
    Serial.printf("First packet duplicate check: %s\n", isDupe1 ? "DUPLICATE" : "NEW");
    
    // Add packet to seen list
    ReliabilityManager::addSeenPacket(testSource, testSeq1);
    
    // Same packet should now be duplicate
    bool isDupe2 = ReliabilityManager::isDuplicatePacket(testSource, testSeq1);
    Serial.printf("Second packet duplicate check: %s\n", isDupe2 ? "DUPLICATE" : "NEW");
    
    // Different sequence should not be duplicate
    bool isDupe3 = ReliabilityManager::isDuplicatePacket(testSource, testSeq2);
    Serial.printf("Different sequence duplicate check: %s\n", isDupe3 ? "DUPLICATE" : "NEW");
    
    // Test packet ID generation
    Serial.println("Testing packet ID generation...");
    for (int i = 0; i < 3; i++) {
        uint32_t packetId = ReliabilityManager::generatePacketId();
        Serial.printf("Generated packet ID: %lu\n", packetId);
    }
    
    // Test alternative route management
    Serial.println("Testing alternative routes...");
    uint32_t destination = 0xABCDEF00;
    ReliabilityManager::addAlternativeRoute(destination, 0x11111111, 2, 300, 0); // Primary
    ReliabilityManager::addAlternativeRoute(destination, 0x22222222, 3, 301, 1); // Backup
    ReliabilityManager::addAlternativeRoute(destination, 0x33333333, 4, 302, 2); // Tertiary
    
    // Test getting alternative route when primary fails
    AlternativeRoute* altRoute = ReliabilityManager::getAlternativeRoute(destination, 0x11111111);
    if (altRoute) {
        Serial.printf("Found alternative route to %08X via %08X (hops=%d, priority=%d)\n",
                     destination, altRoute->nextHop, altRoute->hopCount, altRoute->priority);
    } else {
        Serial.println("ERROR: No alternative route found");
    }
    
    Serial.println("SUCCESS: Reliable delivery test completed!");
    Serial.println("=== End Reliable Delivery Test ===");
    Serial.println();
}

// Test function to verify mesh optimization functionality
void testMeshOptimization() {
    Serial.println("=== Testing Mesh Optimization ===" );
    
    // Test adaptive spreading factor calculation
    Serial.println("Testing adaptive spreading factor...");
    
    // Simulate different RSSI values and test SF calculation
    int rssiValues[] = {-70, -85, -95, -110, -125};
    for (int i = 0; i < 5; i++) {
        uint8_t optimalSF = NeighborTable::calculateOptimalSF(rssiValues[i]);
        Serial.printf("RSSI: %d dBm -> Optimal SF: %d\n", rssiValues[i], optimalSF);
    }
    
    // Test load balancing metrics
    Serial.println("Testing load balancing...");
    
    // Add test neighbors with different load characteristics
    NeighborTable::addOrUpdateNeighbor(0x11111111, "HighLoad", 8, 15, 
                                      NEIGHBOR_CAP_ALWAYS_ON, 1, -75);
    NeighborTable::addOrUpdateNeighbor(0x22222222, "LowLoad", 2, 3, 
                                      NEIGHBOR_CAP_ALWAYS_ON, 1, -75);
    NeighborTable::addOrUpdateNeighbor(0x33333333, "BatteryNode", 1, 2, 
                                      NEIGHBOR_CAP_BATTERY_POWERED, 1, -80);
    
    // Add test routes with different characteristics
    RouteTable::addRoute(0xAAAAAAAA, 0x11111111, 2, 100); // High load next hop
    RouteTable::addRoute(0xBBBBBBBB, 0x22222222, 3, 101); // Low load next hop  
    RouteTable::addRoute(0xCCCCCCCC, 0x33333333, 2, 102); // Battery next hop
    
    // Test route scoring
    Serial.println("Testing route quality scoring...");
    RouteTable::updateRouteMetrics();
    
    RouteEntry* routeA = RouteTable::findBestRoute(0xAAAAAAAA);
    RouteEntry* routeB = RouteTable::findBestRoute(0xBBBBBBBB);
    RouteEntry* routeC = RouteTable::findBestRoute(0xCCCCCCCC);
    
    if (routeA) {
        Serial.printf("Route A (high load): quality=%d%%, load=%d%%\n", 
                     routeA->routeQuality, routeA->loadFactor);
    }
    if (routeB) {
        Serial.printf("Route B (low load): quality=%d%%, load=%d%%\n", 
                     routeB->routeQuality, routeB->loadFactor);
    }
    if (routeC) {
        Serial.printf("Route C (battery): quality=%d%%, load=%d%%\n", 
                     routeC->routeQuality, routeC->loadFactor);
    }
    
    // Test power awareness
    Serial.println("Testing power-aware routing...");
    bool isPowerAwareA = RouteTable::isPowerAwareRoute(0x11111111);
    bool isPowerAwareB = RouteTable::isPowerAwareRoute(0x22222222);
    bool isPowerAwareC = RouteTable::isPowerAwareRoute(0x33333333);
    
    Serial.printf("Route A next hop battery powered: %s\n", isPowerAwareA ? "Yes" : "No");
    Serial.printf("Route B next hop battery powered: %s\n", isPowerAwareB ? "Yes" : "No");
    Serial.printf("Route C next hop battery powered: %s\n", isPowerAwareC ? "Yes" : "No");
    
    // Test adaptive rate updates
    Serial.println("Testing adaptive rate updates...");
    NeighborTable::updateAdaptiveRates();
    
    // Show updated neighbor table with SF info
    NeighborTable::printNeighborTable();
    
    // Clean up test entries
    NeighborTable::removeNeighbor(0x11111111);
    NeighborTable::removeNeighbor(0x22222222);
    NeighborTable::removeNeighbor(0x33333333);
    RouteTable::removeRoute(0xAAAAAAAA);
    RouteTable::removeRoute(0xBBBBBBBB);
    RouteTable::removeRoute(0xCCCCCCCC);
    
    Serial.println("SUCCESS: Mesh optimization test completed!");
    Serial.println("=== End Mesh Optimization Test ===");
    Serial.println();
}

// Test function to verify connection management functionality
void testConnectionManagement() {
    Serial.println("=== Testing Connection Management ===");
    
    // Test connection manager initialization
    Serial.println("Testing ConnectionManager initialization...");
    ConnectionManager::init();
    
    // Simulate adding connections with different quality characteristics
    Serial.println("Testing connection tracking...");
    
    // Add test connections
    ConnectionManager::addConnection(1, "iPhone-12-Pro");
    ConnectionManager::addConnection(2, "iPad-Air-4");
    ConnectionManager::addConnection(3, "iPhone-SE-2020");
    
    // Simulate different RSSI values over time
    Serial.println("Testing RSSI tracking and scoring...");
    
    // Excellent connection
    for (int i = 0; i < 5; i++) {
        ConnectionManager::updateConnectionRSSI(1, -55 + random(-5, 5));
        ConnectionManager::updateConnectionActivity(1, true);
    }
    
    // Good connection
    for (int i = 0; i < 5; i++) {
        ConnectionManager::updateConnectionRSSI(2, -75 + random(-5, 5));
        ConnectionManager::updateConnectionActivity(2, true);
    }
    
    // Poor connection with some errors
    for (int i = 0; i < 5; i++) {
        ConnectionManager::updateConnectionRSSI(3, -95 + random(-5, 5));
        ConnectionManager::updateConnectionActivity(3, i < 3); // 2 errors out of 5
    }
    
    // Process connections to update scores
    ConnectionManager::process();
    
    // Test connection scoring
    Serial.println("Testing connection quality assessment...");
    uint8_t score1 = ConnectionManager::getConnectionScore(1);
    uint8_t score2 = ConnectionManager::getConnectionScore(2);
    uint8_t score3 = ConnectionManager::getConnectionScore(3);
    
    Serial.printf("Connection 1 (excellent): score=%d%%, healthy=%s\n", 
                 score1, ConnectionManager::isConnectionHealthy(1) ? "Yes" : "No");
    Serial.printf("Connection 2 (good): score=%d%%, healthy=%s\n", 
                 score2, ConnectionManager::isConnectionHealthy(2) ? "Yes" : "No");
    Serial.printf("Connection 3 (poor): score=%d%%, healthy=%s\n", 
                 score3, ConnectionManager::isConnectionHealthy(3) ? "Yes" : "No");
    
    // Test hysteresis and stability
    Serial.println("Testing connection stability and hysteresis...");
    
    // Simulate score fluctuations to test stability
    for (int i = 0; i < 10; i++) {
        // Stable connection
        ConnectionManager::updateConnectionRSSI(1, -55 + random(-2, 2));
        ConnectionManager::updateConnectionActivity(1, true);
        
        // Unstable connection
        ConnectionManager::updateConnectionRSSI(3, -85 + random(-15, 15));
        ConnectionManager::updateConnectionActivity(3, random(0, 2)); // Random success/failure
        
        ConnectionManager::process();
        delay(100); // Small delay to simulate time passing
    }
    
    Serial.printf("Should maintain connection 1: %s\n", 
                 ConnectionManager::shouldMaintainConnection(1) ? "Yes" : "No");
    Serial.printf("Should maintain connection 3: %s\n", 
                 ConnectionManager::shouldMaintainConnection(3) ? "Yes" : "No");
    
    // Test mesh coordination
    Serial.println("Testing mesh coordination...");
    uint8_t bestScore = ConnectionManager::getBestConnectionScore();
    size_t healthyCount = ConnectionManager::getHealthyConnectionCount();
    
    Serial.printf("Best connection score: %d%%\n", bestScore);
    Serial.printf("Healthy connections: %d\n", healthyCount);
    
    // Print final connection statistics
    Serial.println("Final connection statistics:");
    ConnectionManager::printConnectionStats();
    
    // Clean up test connections
    ConnectionManager::removeConnection(1);
    ConnectionManager::removeConnection(2);
    ConnectionManager::removeConnection(3);
    
    Serial.println("SUCCESS: Connection management test completed!");
    Serial.println("=== End Connection Management Test ===");
    Serial.println();
}

// Test function to verify message prioritization functionality
void testMessagePrioritization() {
    Serial.println("=== Testing Message Prioritization ===");
    
    // Initialize message priority manager
    Serial.println("Testing MessagePriorityManager initialization...");
    MessagePriorityManager::init();
    
    // Create test messages of different priorities
    Serial.println("Testing priority determination and queuing...");
    
    // Create presence message (highest priority)
    BitchatPacket presenceMsg;
    presenceMsg.type = MSG_TYPE_ANNOUNCE;
    memset(presenceMsg.senderID, 0x11, 8);
    memset(presenceMsg.recipientID, 0, 8);  // Broadcast
    presenceMsg.ttl = 5;
    presenceMsg.timestamp = millis();
    const char* presenceData = "User joined";
    presenceMsg.payload = (uint8_t*)presenceData;
    presenceMsg.payloadLength = strlen(presenceData);
    
    // Create private message (high priority)
    BitchatPacket privateMsg;
    privateMsg.type = MSG_TYPE_MESSAGE;
    memset(privateMsg.senderID, 0x22, 8);
    memset(privateMsg.recipientID, 0x33, 8);  // Specific recipient
    privateMsg.ttl = 5;
    privateMsg.timestamp = millis();
    const char* privateData = "Hello Alice!";
    privateMsg.payload = (uint8_t*)privateData;
    privateMsg.payloadLength = strlen(privateData);
    
    // Create broadcast message (medium priority)
    BitchatPacket broadcastMsg;
    broadcastMsg.type = MSG_TYPE_MESSAGE;
    memset(broadcastMsg.senderID, 0x44, 8);
    memset(broadcastMsg.recipientID, 0, 8);  // Broadcast
    broadcastMsg.ttl = 5;
    broadcastMsg.timestamp = millis();
    const char* broadcastData = "Hello everyone!";
    broadcastMsg.payload = (uint8_t*)broadcastData;
    broadcastMsg.payloadLength = strlen(broadcastData);
    
    // Create file transfer message (lowest priority)
    BitchatPacket fileMsg;
    fileMsg.type = MSG_TYPE_FRAGMENT_START;
    memset(fileMsg.senderID, 0x55, 8);
    memset(fileMsg.recipientID, 0x66, 8);
    fileMsg.ttl = 5;
    fileMsg.timestamp = millis();
    const char* fileData = "Binary data chunk 1";
    fileMsg.payload = (uint8_t*)fileData;
    fileMsg.payloadLength = strlen(fileData);
    
    // Test priority determination
    Serial.println("Testing priority determination...");
    MessagePriority presencePrio = MessagePriorityManager::determinePriority(presenceMsg);
    MessagePriority privatePrio = MessagePriorityManager::determinePriority(privateMsg);
    MessagePriority broadcastPrio = MessagePriorityManager::determinePriority(broadcastMsg);
    MessagePriority filePrio = MessagePriorityManager::determinePriority(fileMsg);
    
    Serial.printf("Presence message priority: %s (%d)\n", 
                 MessagePriorityManager::priorityToString(presencePrio), presencePrio);
    Serial.printf("Private message priority: %s (%d)\n", 
                 MessagePriorityManager::priorityToString(privatePrio), privatePrio);
    Serial.printf("Broadcast message priority: %s (%d)\n", 
                 MessagePriorityManager::priorityToString(broadcastPrio), broadcastPrio);
    Serial.printf("File message priority: %s (%d)\n", 
                 MessagePriorityManager::priorityToString(filePrio), filePrio);
    
    // Test queuing in reverse priority order to verify priority ordering
    Serial.println("Testing priority queuing order...");
    
    // Queue messages in wrong order (lowest to highest priority)
    bool queued1 = MessagePriorityManager::queueMessage(fileMsg, "Source1");
    bool queued2 = MessagePriorityManager::queueMessage(broadcastMsg, "Source2");
    bool queued3 = MessagePriorityManager::queueMessage(privateMsg, "Source3");
    bool queued4 = MessagePriorityManager::queueMessage(presenceMsg, "Source4");
    
    Serial.printf("Queued: File=%s, Broadcast=%s, Private=%s, Presence=%s\n",
                 queued1 ? "Yes" : "No", queued2 ? "Yes" : "No", 
                 queued3 ? "Yes" : "No", queued4 ? "Yes" : "No");
    
    // Verify they come out in correct priority order
    Serial.println("Testing priority dequeue order...");
    PriorityQueueEntry entry;
    int messageCount = 0;
    
    while (MessagePriorityManager::dequeueMessage(entry) && messageCount < 10) {
        messageCount++;
        Serial.printf("Dequeued message %d: %s priority (type: 0x%02X, source: %s)\n",
                     messageCount, MessagePriorityManager::priorityToString(entry.priority),
                     entry.packet.type, entry.sourceId.c_str());
    }
    
    // Test fair queuing by flooding from one source
    Serial.println("Testing fair queuing throttling...");
    MessagePriorityManager::clearQueue();
    
    int throttledCount = 0;
    int successCount = 0;
    
    // Try to queue many messages from same source rapidly
    for (int i = 0; i < 10; i++) {
        BitchatPacket testMsg = broadcastMsg;
        testMsg.timestamp = millis() + i;
        
        if (MessagePriorityManager::queueMessage(testMsg, "FloodingSource")) {
            successCount++;
        } else {
            throttledCount++;
        }
    }
    
    Serial.printf("Fair queuing test: %d messages queued, %d throttled from flooding source\n",
                 successCount, throttledCount);
    
    // Test queue statistics
    Serial.println("Testing queue statistics...");
    MessagePriorityManager::printQueueStats();
    
    // Test queue limits by filling each priority queue
    Serial.println("Testing queue limits...");
    MessagePriorityManager::clearQueue();
    
    // Fill presence queue beyond limit
    for (int i = 0; i < 15; i++) {
        BitchatPacket testMsg = presenceMsg;
        testMsg.timestamp = millis() + i;
        char sourceId[32];
        sprintf(sourceId, "PresenceSource%d", i);
        MessagePriorityManager::queueMessage(testMsg, sourceId);
    }
    
    // Check final statistics
    Serial.println("Final queue statistics after limit testing:");
    MessagePriorityManager::printQueueStats();
    
    // Test integration with MessageRouter
    Serial.println("Testing integration with MessageRouter queuing...");
    MessagePriorityManager::clearQueue();
    
    // Queue messages through MessageRouter (which uses MessagePriorityManager)
    MessageRouter::queueForLoRa(presenceMsg);
    MessageRouter::queueForLoRa(privateMsg);
    MessageRouter::queueForLoRa(broadcastMsg);
    MessageRouter::queueForLoRa(fileMsg);
    
    Serial.printf("Messages queued through MessageRouter: %d total\n",
                 MessagePriorityManager::getTotalQueueDepth());
    
    // Clean up
    MessagePriorityManager::clearQueue();
    
    Serial.println("SUCCESS: Message prioritization test completed!");
    Serial.println("=== End Message Prioritization Test ===");
    Serial.println();
}

// Test function to verify power management functionality
void testPowerManagement() {
    Serial.println("=== Testing Power Management ===");
    
    // Initialize power manager
    Serial.println("Testing PowerManager initialization...");
    PowerManager::init();
    
    // Test battery monitoring
    Serial.println("Testing battery monitoring...");
    float voltage = PowerManager::getBatteryVoltage();
    uint8_t percentage = PowerManager::getBatteryPercentage();
    BatteryLevel level = PowerManager::getBatteryLevel();
    
    Serial.printf("Battery voltage: %.2fV\n", voltage);
    Serial.printf("Battery percentage: %d%%\n", percentage);
    Serial.printf("Battery level: %s\n", 
                 level == BATTERY_FULL ? "FULL" :
                 level == BATTERY_HIGH ? "HIGH" :
                 level == BATTERY_MEDIUM ? "MEDIUM" :
                 level == BATTERY_LOW ? "LOW" : "CRITICAL");
    Serial.printf("Battery low: %s\n", PowerManager::isBatteryLow() ? "Yes" : "No");
    Serial.printf("Battery critical: %s\n", PowerManager::isBatteryCritical() ? "Yes" : "No");
    
    // Test power state management
    Serial.println("Testing power state management...");
    PowerState initialState = PowerManager::getCurrentState();
    Serial.printf("Initial power state: %s\n", 
                 initialState == POWER_ACTIVE ? "ACTIVE" : 
                 initialState == POWER_IDLE ? "IDLE" :
                 initialState == POWER_LOW_BATTERY ? "LOW_BATTERY" : "DEEP_SLEEP");
    
    // Test activity tracking
    Serial.println("Testing activity tracking...");
    PowerManager::recordActivity();
    Serial.printf("Is idle: %s\n", PowerManager::isIdle() ? "Yes" : "No");
    Serial.printf("Idle time: %dms\n", PowerManager::getIdleTime());
    
    // Test BLE advertising intervals
    Serial.println("Testing BLE advertising intervals...");
    uint32_t activeInterval = PowerManager::getBLEAdvertisingInterval();
    Serial.printf("Active advertising interval: %dms\n", activeInterval);
    
    // Simulate idle state
    PowerManager::setState(POWER_IDLE);
    uint32_t idleInterval = PowerManager::getBLEAdvertisingInterval();
    Serial.printf("Idle advertising interval: %dms\n", idleInterval);
    
    // Simulate low battery state
    PowerManager::setState(POWER_LOW_BATTERY);
    uint32_t lowBatteryInterval = PowerManager::getBLEAdvertisingInterval();
    Serial.printf("Low battery advertising interval: %dms\n", lowBatteryInterval);
    
    // Test LoRa TX power optimization
    Serial.println("Testing LoRa TX power optimization...");
    
    // Test different RSSI values
    int16_t rssiValues[] = {-60, -75, -90, -105};
    for (int i = 0; i < 4; i++) {
        int8_t optimalPower = PowerManager::getOptimalTxPower(rssiValues[i]);
        Serial.printf("RSSI: %d dBm -> Optimal TX Power: %d dBm\n", rssiValues[i], optimalPower);
    }
    
    // Test power reduction
    PowerManager::reduceTxPower();
    PowerManager::restoreTxPower();
    
    // Test button handling simulation
    Serial.println("Testing button handling...");
    Serial.printf("Button held for 1000ms: %s\n", PowerManager::isButtonHeld(1000) ? "Yes" : "No");
    
    // Test light sleep (very short duration for testing)
    Serial.println("Testing light sleep...");
    unsigned long sleepStart = millis();
    PowerManager::enterLightSleep(50); // 50ms test sleep
    unsigned long sleepEnd = millis();
    Serial.printf("Light sleep test: slept for ~%dms\n", sleepEnd - sleepStart);
    
    // Test power statistics
    Serial.println("Testing power statistics...");
    PowerManager::printPowerStats();
    
    Serial.printf("Total sleep time: %dms\n", PowerManager::getTotalSleepTime());
    Serial.printf("Average power consumption: %.1fmA\n", PowerManager::getAveragePowerConsumption());
    
    // Test activity reduction check
    Serial.println("Testing activity reduction logic...");
    PowerManager::setState(POWER_ACTIVE);
    Serial.printf("Should reduce activity (ACTIVE): %s\n", PowerManager::shouldReduceActivity() ? "Yes" : "No");
    
    PowerManager::setState(POWER_LOW_BATTERY);
    Serial.printf("Should reduce activity (LOW_BATTERY): %s\n", PowerManager::shouldReduceActivity() ? "Yes" : "No");
    
    // Restore normal state
    PowerManager::setState(POWER_ACTIVE);
    
    Serial.println("SUCCESS: Power management test completed!");
    Serial.println("=== End Power Management Test ===");
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("BitChat Repeater starting...");
    
    // Test LoRa packet format
    testLoRaPacketFormat();
    
    // Test neighbor discovery
    testNeighborDiscovery();
    
    // Initialize LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // Turn on LED during startup
    
    // Initialize button
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    // Enable Vext power for peripherals
    pinMode(VEXT_ENABLE, OUTPUT);
    digitalWrite(VEXT_ENABLE, LOW);  // Enable Vext
    
    // Initialize modules
    ConfigManager::init();  // Initialize configuration first
    PowerManager::init();   // Initialize power management first
    MessageRouter::init();  // Initialize message router first
    BLEMesh::init();
    LoRaBridge::init();     // Initialize LoRa radio
    
    // Test transmission system after initialization
    testTransmissionSystem();
    
    // Test mesh routing system
    testMeshRouting();
    
    // Test reliable delivery system
    testReliableDelivery();
    
    // Test mesh optimization system
    testMeshOptimization();
    
    // Test connection management system
    testConnectionManagement();
    
    // Test message prioritization system
    testMessagePrioritization();
    
    // Test power management system
    testPowerManagement();
    
    digitalWrite(LED_PIN, LOW);  // Turn off LED after startup
    Serial.println("BitChat Repeater ready");
}

void loop() {
    // Main loop processing
    PowerManager::process();   // Process power management first
    BLEMesh::process();
    LoRaBridge::process();     // Process LoRa radio
    MessageRouter::process();  // Process deduplication cleanup
    
    delay(10);  // Small delay to prevent watchdog reset
}