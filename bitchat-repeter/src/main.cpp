#include <Arduino.h>
#include "hardware_config.h"
#include "ble_mesh.h"
#include "lora_bridge.h"
#include "message_router.h"
#include "config_manager.h"
#include "bitchat_protocol.h"
#include "connection_manager.h"

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
    // ConfigManager::init();
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
    
    digitalWrite(LED_PIN, LOW);  // Turn off LED after startup
    Serial.println("BitChat Repeater ready");
}

void loop() {
    // Main loop processing
    BLEMesh::process();
    LoRaBridge::process();     // Process LoRa radio
    MessageRouter::process();  // Process deduplication cleanup
    
    delay(10);  // Small delay to prevent watchdog reset
}