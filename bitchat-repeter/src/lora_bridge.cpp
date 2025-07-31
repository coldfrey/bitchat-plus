#include "communication/lora_bridge.h"
#include "communication/message_router.h"
#include <Arduino.h>
#include <algorithm>
#include <esp_random.h>

// NeighborTable static member definitions
std::map<uint32_t, NeighborEntry> NeighborTable::neighbors;

// RouteTable static member definitions
std::map<uint32_t, RouteEntry> RouteTable::routes;
std::map<uint32_t, RouteRequestEntry> RouteTable::pendingRequests;
uint32_t RouteTable::nextRequestId = 1;

// ReliabilityManager static member definitions
std::map<uint32_t, PendingAckEntry> ReliabilityManager::pendingAcks;
std::map<uint64_t, MeshDedupeEntry> ReliabilityManager::meshDedupeCache;
std::map<uint32_t, std::vector<AlternativeRoute>> ReliabilityManager::alternativeRoutes;
uint32_t ReliabilityManager::nextPacketId = 1;

// LoRaBridge static member definitions
SX1262 LoRaBridge::radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
bool LoRaBridge::initialized = false;
bool LoRaBridge::receiving = false;
unsigned long LoRaBridge::lastRxCheck = 0;
unsigned long LoRaBridge::lastNeighborAnnouncement = 0;
std::queue<QueuedPacket> LoRaBridge::transmissionQueue;
unsigned long LoRaBridge::lastTransmission = 0;
bool LoRaBridge::channelBusy = false;
unsigned long LoRaBridge::dutyCycleStartTime = 0;
unsigned long LoRaBridge::totalAirTimeMs = 0;
uint32_t LoRaBridge::ownSequenceNumber = 1;
unsigned long LoRaBridge::lastOptimizationUpdate = 0;
uint8_t LoRaBridge::currentSpreadingFactor = 9;
bool LoRaBridge::adaptiveRatesEnabled = true;
int8_t LoRaBridge::currentTxPower = TX_POWER;
bool LoRaBridge::adaptivePowerEnabled = true;
int LoRaBridge::lastRSSI = 0;
float LoRaBridge::lastSNR = 0.0;
unsigned long LoRaBridge::txCount = 0;
unsigned long LoRaBridge::rxCount = 0;
std::atomic<bool> LoRaBridge::receivedFlag{false};
std::map<uint32_t, RateLimitEntry> LoRaBridge::rateLimitTable;

// Configuration constants
const float LoRaBridge::FREQUENCY = 915.0;           // MHz (US ISM band)
const float LoRaBridge::BANDWIDTH = 125.0;          // kHz
const uint8_t LoRaBridge::SPREADING_FACTOR = 9;     // SF9 for good range/reliability balance
const uint8_t LoRaBridge::CODING_RATE = 5;          // 4/5 coding rate
const int8_t LoRaBridge::TX_POWER = 20;             // dBm (maximum for most regions)
const uint8_t LoRaBridge::SYNC_WORD = 0x12;         // Private network sync word
const unsigned long LoRaBridge::NEIGHBOR_ANNOUNCE_INTERVAL_MS;
const size_t LoRaBridge::MAX_QUEUE_SIZE;
const unsigned long LoRaBridge::CAD_TIMEOUT_MS;
const unsigned long LoRaBridge::MIN_BACKOFF_MS;
const unsigned long LoRaBridge::MAX_BACKOFF_MS;
const uint8_t LoRaBridge::MAX_RETRIES;
const unsigned long LoRaBridge::DUTY_CYCLE_WINDOW_MS;
const unsigned long LoRaBridge::MAX_AIRTIME_MS;
const unsigned long LoRaBridge::OPTIMIZATION_UPDATE_INTERVAL_MS;

// RouteTable constants
const unsigned long RouteTable::ROUTE_TIMEOUT_MS;
const unsigned long RouteTable::REQUEST_TIMEOUT_MS;

// ReliabilityManager constants
const unsigned long ReliabilityManager::ACK_TIMEOUT_MS;
const unsigned long ReliabilityManager::DEDUPE_TIMEOUT_MS;
const uint8_t ReliabilityManager::MAX_RETRIES;

void LoRaBridge::init() {
    Serial.println("[LORA] Initializing SX1262 LoRa radio module...");
    
    // Initialize SPI pins
    Serial.printf("[LORA] Initializing SPI: SCK=%d, MISO=%d, MOSI=%d, CS=%d\n", 
                 LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    
    // Initialize radio
    Serial.printf("[LORA] Radio pins: CS=%d, DIO1=%d, RST=%d, BUSY=%d\n", 
                 LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
    
    Serial.println("[LORA] Starting radio initialization...");
    int state = radio.begin();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] *** RADIO INITIALIZATION FAILED ***\n");
        Serial.printf("[LORA] Error code: %d\n", state);
        Serial.println("[LORA] Check wiring and power connections!");
        return;
    }
    
    Serial.println("[LORA] Radio hardware initialized successfully");
    Serial.println("[LORA] Configuring radio parameters...");
    
    // Configure radio parameters
    if (!configure()) {
        Serial.println("[LORA] *** RADIO CONFIGURATION FAILED ***");
        return;
    }
    
    // Set up interrupt handler
    radio.setDio1Action(onReceive);
    
    // Start receiving
    startReceive();
    
    // Initialize neighbor table
    NeighborTable::init();
    
    // Initialize route table
    RouteTable::init();
    
    // Initialize reliability manager
    ReliabilityManager::init();
    
    // Initialize duty cycle tracking
    dutyCycleStartTime = millis();
    totalAirTimeMs = 0;
    
    initialized = true;
    Serial.println("LoRa Bridge: Initialization complete");
    Serial.printf("LoRa Bridge: Freq=%.1f MHz, BW=%.1f kHz, SF=%d, CR=4/%d, Power=%d dBm\n",
                 FREQUENCY, BANDWIDTH, SPREADING_FACTOR, CODING_RATE, TX_POWER);
}

void LoRaBridge::process() {
    if (!initialized) {
        return;
    }
    
    // Handle received messages - use atomic compare-and-swap for thread safety
    if (receivedFlag.exchange(false)) {
        handleReceivedMessage();
        
        // Restart receive mode
        startReceive();
    }
    
    // Periodic operations
    unsigned long now = millis();
    
    // Status check every 30 seconds
    if (now - lastRxCheck > 30000) {
        lastRxCheck = now;
        Serial.printf("LoRa Bridge: Stats - TX: %lu, RX: %lu, Last RSSI: %d dBm, SNR: %.1f dB, Neighbors: %d\n",
                     txCount, rxCount, lastRSSI, lastSNR, NeighborTable::getNeighborCount());
    }
    
    // Send neighbor announcement every 60 seconds
    if (now - lastNeighborAnnouncement > NEIGHBOR_ANNOUNCE_INTERVAL_MS) {
        lastNeighborAnnouncement = now;
        sendNeighborAnnouncement();
    }
    
    // Clean up stale neighbors every 2 minutes
    static unsigned long lastNeighborCleanup = 0;
    if (now - lastNeighborCleanup > 120000) {
        lastNeighborCleanup = now;
        NeighborTable::cleanupStaleNeighbors();
    }
    
    // Clean up expired routes every 5 minutes
    static unsigned long lastRouteCleanup = 0;
    if (now - lastRouteCleanup > 300000) {
        lastRouteCleanup = now;
        RouteTable::cleanupExpiredRoutes();
        RouteTable::cleanupExpiredRequests();
    }
    
    // Process reliability timeouts every 100ms
    static unsigned long lastReliabilityCheck = 0;
    if (now - lastReliabilityCheck > 100) {
        lastReliabilityCheck = now;
        processReliabilityTimeouts();
        ReliabilityManager::cleanupExpiredEntries();
    }
    
    // Update network optimization (adaptive rates, load balancing)
    updateNetworkOptimization();
    
    // Process transmission queue
    processTransmissionQueue();
}

bool LoRaBridge::transmit(const BitchatPacket& packet) {
    if (!initialized) {
        Serial.println("LoRa Bridge: Cannot transmit - radio not initialized");
        return false;
    }
    
    // Serialize packet to binary data
    uint8_t buffer[256];
    size_t packetSize = serializePacket(packet, buffer, sizeof(buffer));
    
    if (packetSize == 0) {
        Serial.println("LoRa Bridge: Failed to serialize packet for transmission");
        return false;
    }
    
    // Temporarily stop receiving
    receiving = false;
    
    // Transmit the packet
    int state = radio.transmit(buffer, packetSize);
    
    bool success = (state == RADIOLIB_ERR_NONE);
    if (success) {
        txCount++;
        Serial.printf("LoRa Bridge: Transmitted packet (type=0x%02X, size=%d bytes)\n", 
                     packet.type, packetSize);
    } else {
        Serial.printf("LoRa Bridge: Transmission failed, code %d\n", state);
    }
    
    // Restart receiving
    startReceive();
    
    return success;
}

bool LoRaBridge::transmitLoRaPacket(const LoRaPacket& packet) {
    // Queue the packet instead of transmitting directly
    return queueLoRaPacket(packet);
}

bool LoRaBridge::queueLoRaPacket(const LoRaPacket& packet) {
    Serial.println("\n[LORA-TX] *** QUEUING LoRa PACKET ***");
    
    if (!initialized) {
        Serial.println("[LORA-TX] ❌ Cannot queue - LoRa radio not initialized");
        Serial.println("[LORA-TX] *** END QUEUE ATTEMPT ***\n");
        return false;
    }
    
    // Determine packet type string
    const char* pktTypeStr = "UNKNOWN";
    switch (packet.type) {
        case LORA_PKT_DATA: pktTypeStr = "DATA"; break;
        case LORA_PKT_NEIGHBOR_ANNOUNCE: pktTypeStr = "NEIGHBOR_ANNOUNCE"; break;
        case LORA_PKT_ROUTE_REQUEST: pktTypeStr = "ROUTE_REQUEST"; break;
        case LORA_PKT_ROUTE_REPLY: pktTypeStr = "ROUTE_REPLY"; break;
        case LORA_PKT_MESH_ACK: pktTypeStr = "MESH_ACK"; break;
    }
    
    Serial.println("[LORA-TX] === PACKET TO QUEUE ===");
    Serial.printf("[LORA-TX] Type: %s (0x%02X)\n", pktTypeStr, packet.type);
    Serial.printf("[LORA-TX] Source: %08X\n", packet.srcRepeater);
    Serial.printf("[LORA-TX] Destination: %08X\n", packet.destRepeater);
    Serial.printf("[LORA-TX] Next Hop: %08X\n", packet.nextHop);
    Serial.printf("[LORA-TX] Payload Length: %d bytes\n", packet.payloadLen);
    Serial.printf("[LORA-TX] Current Queue Size: %d\n", transmissionQueue.size());
    
    // Apply rate limiting to prevent DoS attacks
    if (!checkRateLimit(packet.srcRepeater)) {
        Serial.printf("[LORA-TX] ❌ DROPPING packet from %08X due to rate limiting\n", packet.srcRepeater);
        Serial.println("[LORA-TX] *** END QUEUE ATTEMPT ***\n");
        return false;
    }
    
    // Check if queue is full
    if (transmissionQueue.size() >= MAX_QUEUE_SIZE) {
        Serial.printf("[LORA-TX] ⚠️ Queue full (%d packets), dropping oldest\n", transmissionQueue.size());
        transmissionQueue.pop(); // Drop oldest packet
    }
    
    // Add packet to queue
    QueuedPacket queuedPacket(packet);
    transmissionQueue.push(queuedPacket);
    
    // Update rate limiting counter
    updateRateLimit(packet.srcRepeater);
    
    Serial.printf("[LORA-TX] ✅ Successfully queued packet (new queue size: %d)\n", transmissionQueue.size());
    Serial.printf("[LORA-TX] Next transmission attempt in ~%lu ms\n", queuedPacket.nextAttempt - millis());
    Serial.println("[LORA-TX] *** END QUEUE ATTEMPT ***\n");
    
    return true;
}

void LoRaBridge::startReceive() {
    if (!initialized) {
        return;
    }
    
    int state = radio.startReceive();
    if (state == RADIOLIB_ERR_NONE) {
        receiving = true;
        Serial.println("LoRa Bridge: Started continuous receive mode");
    } else {
        Serial.printf("LoRa Bridge: Failed to start receive mode, code %d\n", state);
        receiving = false;
    }
}

bool LoRaBridge::isReceiving() {
    return initialized && receiving;
}

int LoRaBridge::getLastRSSI() {
    return lastRSSI;
}

float LoRaBridge::getLastSNR() {
    return lastSNR;
}

void LoRaBridge::setTxPower(int8_t power) {
    // Clamp power to safe limits
    if (power > TX_POWER) power = TX_POWER;
    if (power < 2) power = 2; // Minimum 2 dBm
    
    if (currentTxPower != power) {
        currentTxPower = power;
        
        // Update radio TX power
        int state = radio.setOutputPower(currentTxPower);
        if (state == RADIOLIB_ERR_NONE) {
            Serial.printf("LoRaBridge: TX power set to %d dBm\n", currentTxPower);
        } else {
            Serial.printf("LoRaBridge: Failed to set TX power to %d dBm, error %d\n", currentTxPower, state);
        }
    }
}

int8_t LoRaBridge::getTxPower() {
    return currentTxPower;
}

void LoRaBridge::setAdaptivePower(bool enabled) {
    adaptivePowerEnabled = enabled;
    Serial.printf("LoRaBridge: Adaptive power %s\n", enabled ? "enabled" : "disabled");
}

bool LoRaBridge::isAdaptivePowerEnabled() {
    return adaptivePowerEnabled;
}

// Interrupt handler - keep it minimal
void LoRaBridge::onReceive() {
    receivedFlag = true;
}

bool LoRaBridge::configure() {
    int state;
    
    Serial.println("[LORA-CFG] Configuring radio parameters...");
    
    // Set frequency
    Serial.printf("[LORA-CFG] Setting frequency: %.1f MHz\n", FREQUENCY);
    state = radio.setFrequency(FREQUENCY);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA-CFG] Failed to set frequency, code %d\n", state);
        return false;
    }
    
    // Set bandwidth
    Serial.printf("[LORA-CFG] Setting bandwidth: %.1f kHz\n", BANDWIDTH);
    state = radio.setBandwidth(BANDWIDTH);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA-CFG] Failed to set bandwidth, code %d\n", state);
        return false;
    }
    
    // Set spreading factor
    Serial.printf("[LORA-CFG] Setting spreading factor: SF%d\n", SPREADING_FACTOR);
    state = radio.setSpreadingFactor(SPREADING_FACTOR);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA-CFG] Failed to set spreading factor, code %d\n", state);
        return false;
    }
    
    // Set coding rate
    Serial.printf("[LORA-CFG] Setting coding rate: 4/%d\n", CODING_RATE);
    state = radio.setCodingRate(CODING_RATE);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA-CFG] Failed to set coding rate, code %d\n", state);
        return false;
    }
    
    // Set output power
    state = radio.setOutputPower(TX_POWER);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("LoRa Bridge: Failed to set output power, code %d\n", state);
        return false;
    }
    
    // Set sync word (private network)
    state = radio.setSyncWord(SYNC_WORD);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("LoRa Bridge: Failed to set sync word, code %d\n", state);
        return false;
    }
    
    // Enable CRC
    state = radio.setCRC(true);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("LoRa Bridge: Failed to enable CRC, code %d\n", state);
        return false;
    }
    
    Serial.println("LoRa Bridge: Radio configuration successful");
    return true;
}

void LoRaBridge::handleReceivedMessage() {
    Serial.println("\n[LORA-RX] *** LoRa MESSAGE RECEIVED ***");
    
    // Read the received data
    uint8_t buffer[256];
    int state = radio.readData(buffer, sizeof(buffer));
    
    if (state < 0) {
        Serial.printf("[LORA-RX] ❌ FAILED to read received data, error code: %d\n", state);
        Serial.println("[LORA-RX] *** END LoRa PROCESSING ***\n");
        return;
    }
    
    size_t receivedSize = state;
    rxCount++;
    
    // Get RSSI and SNR
    lastRSSI = radio.getRSSI();
    lastSNR = radio.getSNR();
    
    Serial.printf("[LORA-RX] Data Length: %d bytes\n", receivedSize);
    Serial.printf("[LORA-RX] Signal Quality - RSSI: %d dBm, SNR: %.1f dB\n", lastRSSI, lastSNR);
    Serial.printf("[LORA-RX] Total RX Count: %lu\n", rxCount);
    Serial.printf("[LORA-RX] Timestamp: %lu ms\n", millis());
    
    // Print raw data dump
    Serial.print("[LORA-RX] Raw Data: ");
    for (size_t i = 0; i < receivedSize; i++) {
        Serial.printf("%02X ", buffer[i]);
        if ((i + 1) % 16 == 0) Serial.print("\n[LORA-RX]           ");
    }
    Serial.println();
    
    // Check if this is a LoRa mesh packet (starts with magic 0xBC)
    if (receivedSize >= 3 && buffer[0] == LORA_MAGIC) {
        Serial.printf("[LORA-RX] ✅ Detected LoRa mesh packet (magic: 0x%02X)\n", buffer[0]);
        handleReceivedLoRaPacket(buffer, receivedSize);
        Serial.println("[LORA-RX] *** END LoRa PROCESSING ***\n");
        return;
    }
    
    // Otherwise, try to parse as BitChat packet
    Serial.println("[LORA-RX] Attempting to parse as BitChat packet...");
    BitchatPacket packet;
    ParseResult result = parsePacket(buffer, receivedSize, packet);
    
    if (result != PARSE_SUCCESS) {
        Serial.printf("[LORA-RX] ❌ FAILED to parse as BitChat packet, error code: %d\n", result);
        Serial.println("[LORA-RX] This might be an unknown packet format or corrupted data");
        Serial.println("[LORA-RX] *** END LoRa PROCESSING ***\n");
        return;
    }
    
    Serial.println("[LORA-RX] ✅ Successfully parsed as BitChat packet!");
    
    // Convert sender ID to hex string for logging
    char senderHex[17];
    for (int i = 0; i < 8; i++) {
        sprintf(senderHex + (i * 2), "%02X", packet.senderID[i]);
    }
    senderHex[16] = '\0';
    
    // Determine message type string
    const char* msgTypeStr = "UNKNOWN";
    switch (packet.type) {
        case MSG_TYPE_ANNOUNCE: msgTypeStr = "ANNOUNCE"; break;
        case MSG_TYPE_LEAVE: msgTypeStr = "LEAVE"; break;
        case MSG_TYPE_MESSAGE: msgTypeStr = "MESSAGE"; break;
        case MSG_TYPE_DELIVERY_ACK: msgTypeStr = "DELIVERY_ACK"; break;
        case MSG_TYPE_PROTOCOL_ACK: msgTypeStr = "PROTOCOL_ACK"; break;
    }
    
    Serial.println("[LORA-RX] === BitChat PACKET DETAILS ===");
    Serial.printf("[LORA-RX] Type: %s (0x%02X)\n", msgTypeStr, packet.type);
    Serial.printf("[LORA-RX] From: %s\n", senderHex);
    Serial.printf("[LORA-RX] TTL: %d\n", packet.ttl);
    Serial.printf("[LORA-RX] Payload Length: %d bytes\n", packet.payloadLength);
    
    // Print payload preview if available
    if (packet.payload && packet.payloadLength > 0) {
        Serial.print("[LORA-RX] Payload Preview: ");
        size_t previewLen = (packet.payloadLength < 32) ? packet.payloadLength : 32;
        for (size_t i = 0; i < previewLen; i++) {
            if (packet.payload[i] >= 32 && packet.payload[i] <= 126) {
                Serial.printf("%c", packet.payload[i]);
            } else {
                Serial.print(".");
            }
        }
        if (packet.payloadLength > 32) Serial.print("...");
        Serial.println();
    }
    
    Serial.println("[LORA-RX] Forwarding to message router for BLE relay...");
    
    // Forward to message router for processing
    MessageRouter::handleLoRaMessage(packet);
    
    Serial.println("[LORA-RX] *** END LoRa PROCESSING ***\n");
}

void LoRaBridge::handleReceivedLoRaPacket(const uint8_t* buffer, size_t receivedSize) {
    Serial.println("[LORA-RX] === Processing LoRa Mesh Packet ===");
    
    // Parse the LoRa packet
    LoRaPacket loraPacket;
    LoRaParseResult result = parseLoRaPacket(buffer, receivedSize, loraPacket);
    
    if (result != LORA_PARSE_SUCCESS) {
        Serial.printf("[LORA-RX] ❌ FAILED to parse LoRa mesh packet, error code: %d\n", result);
        Serial.println("[LORA-RX] Packet might be corrupted or use unsupported format");
        return;
    }
    
    Serial.println("[LORA-RX] ✅ Successfully parsed LoRa mesh packet!");
    
    // Determine packet type string
    const char* pktTypeStr = "UNKNOWN";
    switch (loraPacket.type) {
        case LORA_PKT_DATA: pktTypeStr = "DATA"; break;
        case LORA_PKT_NEIGHBOR_ANNOUNCE: pktTypeStr = "NEIGHBOR_ANNOUNCE"; break;
        case LORA_PKT_ROUTE_REQUEST: pktTypeStr = "ROUTE_REQUEST"; break;
        case LORA_PKT_ROUTE_REPLY: pktTypeStr = "ROUTE_REPLY"; break;
        case LORA_PKT_MESH_ACK: pktTypeStr = "MESH_ACK"; break;
    }
    
    Serial.println("[LORA-RX] === LoRa PACKET DETAILS ===");
    Serial.printf("[LORA-RX] Type: %s (0x%02X)\n", pktTypeStr, loraPacket.type);
    Serial.printf("[LORA-RX] Source Repeater: %08X\n", loraPacket.srcRepeater);
    Serial.printf("[LORA-RX] Dest Repeater: %08X\n", loraPacket.destRepeater);
    Serial.printf("[LORA-RX] Next Hop: %08X\n", loraPacket.nextHop);
    Serial.printf("[LORA-RX] Hop Count: %d/%d\n", loraPacket.hopCount, loraPacket.maxHops);
    Serial.printf("[LORA-RX] Sequence Number: %04X\n", loraPacket.seqNum);
    Serial.printf("[LORA-RX] Payload Length: %d bytes\n", loraPacket.payloadLen);
    
    // Handle different LoRa packet types
    switch (loraPacket.type) {
        case LORA_PKT_NEIGHBOR_ANNOUNCE:
            handleNeighborAnnouncement(loraPacket, lastRSSI);
            break;
            
        case LORA_PKT_DATA:
            // Route data packet to its destination
            routeDataPacket(loraPacket);
            break;
            
        case LORA_PKT_ROUTE_REQUEST:
            handleRouteRequest(loraPacket, lastRSSI);
            break;
            
        case LORA_PKT_ROUTE_REPLY:
            handleRouteReply(loraPacket, lastRSSI);
            break;
            
        case LORA_PKT_MESH_ACK:
            handleMeshAck(loraPacket, lastRSSI);
            break;
            
        default:
            Serial.printf("LoRa Bridge: Unknown LoRa packet type 0x%02X\n", loraPacket.type);
            break;
    }
}

// ================ NeighborTable Implementation ================

void NeighborTable::init() {
    neighbors.clear();
    Serial.println("NeighborTable: Initialized");
}

void NeighborTable::addOrUpdateNeighbor(uint32_t repeaterId, const std::string& name, 
                                        uint8_t connectedDevices, uint8_t queueDepth, 
                                        uint8_t capabilities, uint8_t firmwareVersion, int rssi) {
    
    unsigned long now = millis();
    auto it = neighbors.find(repeaterId);
    
    if (it != neighbors.end()) {
        // Update existing neighbor
        NeighborEntry& entry = it->second;
        entry.name = name;
        entry.connectedDevices = connectedDevices;
        entry.queueDepth = queueDepth;
        entry.capabilities = capabilities;
        entry.firmwareVersion = firmwareVersion;
        entry.lastSeen = now;
        
        // Update rolling average RSSI
        if (entry.packetCount < 10) {
            entry.rssiSum += rssi;
            entry.packetCount++;
        } else {
            // Rolling average of last 10 packets
            entry.rssiSum = entry.rssiSum - entry.avgRSSI + rssi;
            entry.packetCount = 10;
        }
        entry.avgRSSI = entry.rssiSum / entry.packetCount;
        entry.linkQuality = calculateLinkQuality(entry.avgRSSI);
        
        Serial.printf("NeighborTable: Updated %08X (%s) - RSSI:%d, Avg:%d, Quality:%d%%\n",
                     repeaterId, name.c_str(), rssi, entry.avgRSSI, entry.linkQuality);
    } else {
        // Add new neighbor
        NeighborEntry entry;
        entry.repeaterId = repeaterId;
        entry.name = name;
        entry.connectedDevices = connectedDevices;
        entry.queueDepth = queueDepth;
        entry.capabilities = capabilities;
        entry.firmwareVersion = firmwareVersion;
        entry.lastSeen = now;
        entry.avgRSSI = rssi;
        entry.rssiSum = rssi;
        entry.packetCount = 1;
        entry.linkQuality = calculateLinkQuality(rssi);
        
        neighbors[repeaterId] = entry;
        Serial.printf("NeighborTable: Added %08X (%s) - RSSI:%d, Quality:%d%%\n",
                     repeaterId, name.c_str(), rssi, entry.linkQuality);
    }
}

void NeighborTable::removeNeighbor(uint32_t repeaterId) {
    auto it = neighbors.find(repeaterId);
    if (it != neighbors.end()) {
        Serial.printf("NeighborTable: Removed %08X (%s)\n", 
                     it->second.repeaterId, it->second.name.c_str());
        neighbors.erase(it);
    }
}

void NeighborTable::cleanupStaleNeighbors() {
    unsigned long now = millis();
    auto it = neighbors.begin();
    
    while (it != neighbors.end()) {
        if (now - it->second.lastSeen > NEIGHBOR_TIMEOUT_MS) {
            Serial.printf("NeighborTable: Removing stale neighbor %08X (%s)\n",
                         it->second.repeaterId, it->second.name.c_str());
            it = neighbors.erase(it);
        } else {
            ++it;
        }
    }
}

NeighborEntry* NeighborTable::getNeighbor(uint32_t repeaterId) {
    auto it = neighbors.find(repeaterId);
    return (it != neighbors.end()) ? &it->second : nullptr;
}

std::map<uint32_t, NeighborEntry>& NeighborTable::getAllNeighbors() {
    return neighbors;
}

size_t NeighborTable::getNeighborCount() {
    return neighbors.size();
}

void NeighborTable::printNeighborTable() {
    Serial.printf("=== Neighbor Table (%d neighbors) ===\n", neighbors.size());
    Serial.println("RepeaterId   Name           Devices Queue Cap Firmware RSSI Quality LastSeen");
    Serial.println("------------ -------------- ------- ----- --- -------- ---- ------- --------");
    
    for (const auto& pair : neighbors) {
        const NeighborEntry& entry = pair.second;
        unsigned long ageSec = (millis() - entry.lastSeen) / 1000;
        
        Serial.printf("%08X     %-14s %7d %5d %3d %8d %4d %6d%% %6lus\n",
                     entry.repeaterId, entry.name.c_str(), entry.connectedDevices,
                     entry.queueDepth, entry.capabilities, entry.firmwareVersion,
                     entry.avgRSSI, entry.linkQuality, ageSec);
    }
    Serial.println("=====================================");
}

uint8_t NeighborTable::calculateLinkQuality(int avgRSSI) {
    // Convert RSSI to link quality percentage
    // Excellent: -50 to -70 dBm = 100%
    // Good: -70 to -85 dBm = 80-100%
    // Fair: -85 to -100 dBm = 50-80%
    // Poor: -100 to -120 dBm = 0-50%
    
    if (avgRSSI >= -70) {
        return 100;
    } else if (avgRSSI >= -85) {
        return 80 + ((avgRSSI + 85) * 20 / 15);
    } else if (avgRSSI >= -100) {
        return 50 + ((avgRSSI + 100) * 30 / 15);
    } else if (avgRSSI >= -120) {
        return (avgRSSI + 120) * 50 / 20;
    } else {
        return 0;
    }
}

uint8_t NeighborTable::getOptimalSpreadingFactor(uint32_t repeaterId) {
    auto it = neighbors.find(repeaterId);
    if (it != neighbors.end()) {
        return it->second.optimalSF;
    }
    return 9; // Default SF
}

void NeighborTable::updateAdaptiveRates() {
    unsigned long now = millis();
    
    for (auto& pair : neighbors) {
        NeighborEntry& neighbor = pair.second;
        
        // Update SF if enough time has passed and we have recent RSSI data
        if (now - neighbor.lastSFUpdate > SF_UPDATE_INTERVAL_MS && neighbor.packetCount > 0) {
            uint8_t newSF = calculateOptimalSF(neighbor.avgRSSI);
            
            if (newSF != neighbor.optimalSF) {
                Serial.printf("NeighborTable: Updated SF for %08X from %d to %d (RSSI: %d)\n",
                             neighbor.repeaterId, neighbor.optimalSF, newSF, neighbor.avgRSSI);
                neighbor.optimalSF = newSF;
            }
            
            neighbor.lastSFUpdate = now;
        }
    }
}

uint8_t NeighborTable::calculateOptimalSF(int avgRSSI) {
    // Adaptive spreading factor based on RSSI
    // Strong links (>-80 dBm): Use SF7 for faster transmission
    // Medium links (-80 to -100 dBm): Use SF9 for balanced range/speed
    // Weak links (<-100 dBm): Use SF10 for maximum range
    
    if (avgRSSI > LoRaBridge::RSSI_THRESHOLD_STRONG) {
        return LoRaBridge::SF_STRONG_LINK;  // SF7
    } else if (avgRSSI > LoRaBridge::RSSI_THRESHOLD_MEDIUM) {
        return LoRaBridge::SF_MEDIUM_LINK;  // SF9
    } else {
        return LoRaBridge::SF_WEAK_LINK;    // SF10
    }
}

// ================ LoRaBridge Neighbor Discovery ================

void LoRaBridge::sendNeighborAnnouncement() {
    if (!initialized) {
        return;
    }
    
    LoRaPacket packet;
    packet.type = LORA_PKT_NEIGHBOR_ANNOUNCE;
    packet.srcRepeater = getRepeaterID();
    packet.destRepeater = LORA_DEST_BROADCAST;
    packet.nextHop = LORA_DEST_BROADCAST;
    packet.hopCount = 0;
    packet.maxHops = 1; // Don't forward neighbor announcements
    packet.seqNum = millis() & 0xFFFF; // Use timestamp as sequence number
    
    // Create announcement payload
    struct __attribute__((packed)) NeighborAnnouncement {
        uint32_t repeaterId;
        char name[16];
        uint8_t connectedDevices;
        uint8_t queueDepth;
        uint8_t capabilities;
        uint8_t firmwareVersion;
    } announcement;
    
    announcement.repeaterId = packet.srcRepeater;
    snprintf(announcement.name, sizeof(announcement.name), "Repeater-%02X", 
             (unsigned int)(packet.srcRepeater & 0xFF));
    announcement.connectedDevices = 0; // TODO: Get from BLE mesh
    announcement.queueDepth = 0; // TODO: Get from message router
    announcement.capabilities = NEIGHBOR_CAP_ALWAYS_ON; // TODO: Configure based on power source
    announcement.firmwareVersion = 1;
    
    packet.payloadLen = sizeof(announcement);
    memcpy(packet.payload, &announcement, sizeof(announcement));
    
    if (transmitLoRaPacket(packet)) {
        Serial.printf("LoRa Bridge: Sent neighbor announcement (ID: %08X)\n", packet.srcRepeater);
    }
}

void LoRaBridge::handleNeighborAnnouncement(const LoRaPacket& packet, int rssi) {
    struct __attribute__((packed)) NeighborAnnouncement {
        uint32_t repeaterId;
        char name[16];
        uint8_t connectedDevices;
        uint8_t queueDepth;
        uint8_t capabilities;
        uint8_t firmwareVersion;
    };
    
    // Validate payload size before casting to prevent buffer overflow
    if (packet.payloadLen < sizeof(NeighborAnnouncement)) {
        Serial.printf("LoRa Bridge: Invalid neighbor announcement - payload too small (%d < %d)\n", 
                     packet.payloadLen, sizeof(NeighborAnnouncement));
        return;
    }
    
    // Additional bounds check to ensure payload pointer is valid
    if (packet.payload == nullptr) {
        Serial.println("LoRa Bridge: Invalid neighbor announcement - null payload");
        return;
    }
    
    const NeighborAnnouncement* announcement = 
        reinterpret_cast<const NeighborAnnouncement*>(packet.payload);
    
    // Ensure name is null-terminated
    std::string name(announcement->name, 
                    strnlen(announcement->name, sizeof(announcement->name)));
    
    // Don't add ourselves to the neighbor table
    if (announcement->repeaterId == getRepeaterID()) {
        return;
    }
    
    // Add or update neighbor
    NeighborTable::addOrUpdateNeighbor(
        announcement->repeaterId,
        name,
        announcement->connectedDevices,
        announcement->queueDepth,
        announcement->capabilities,
        announcement->firmwareVersion,
        rssi
    );
}

// ================ LoRaBridge Transmission Management ================

void LoRaBridge::processTransmissionQueue() {
    if (!initialized || transmissionQueue.empty()) {
        return;
    }
    
    unsigned long now = millis();
    
    // Get the next packet to transmit
    QueuedPacket& queuedPacket = transmissionQueue.front();
    
    // Check if it's time to attempt transmission
    if (now < queuedPacket.nextAttempt) {
        return;
    }
    
    // Check if packet has exceeded max retries
    if (queuedPacket.retryCount >= MAX_RETRIES) {
        Serial.printf("LoRa Bridge: Dropping packet after %d retries\n", MAX_RETRIES);
        transmissionQueue.pop();
        return;
    }
    
    // Check if channel is clear using CAD
    if (!isChannelClear()) {
        // Channel busy, apply exponential backoff
        unsigned long backoff = getRandomBackoff(queuedPacket.retryCount);
        queuedPacket.nextAttempt = now + backoff;
        queuedPacket.retryCount++;
        
        Serial.printf("LoRa Bridge: Channel busy, backing off %lu ms (retry %d)\n", 
                     backoff, queuedPacket.retryCount);
        return;
    }
    
    // Calculate airtime for this packet
    uint8_t buffer[256];
    size_t packetSize = serializeLoRaPacket(queuedPacket.packet, buffer, sizeof(buffer));
    
    if (packetSize == 0) {
        Serial.println("LoRa Bridge: Failed to serialize queued packet, dropping");
        transmissionQueue.pop();
        return;
    }
    
    unsigned long airTime = calculateAirTime(packetSize);
    
    // Check duty cycle before transmitting
    if (isDutyCycleExceeded(airTime)) {
        Serial.printf("LoRa Bridge: Duty cycle exceeded, dropping packet (airtime: %lu ms)\n", airTime);
        transmissionQueue.pop();
        return;
    }
    
    // Temporarily stop receiving
    receiving = false;
    
    // Transmit the packet
    int state = radio.transmit(buffer, packetSize);
    
    bool success = (state == RADIOLIB_ERR_NONE);
    if (success) {
        txCount++;
        updateDutyCycle(airTime);
        lastTransmission = now;
        
        Serial.printf("LoRa Bridge: Transmitted packet (type=0x%02X, size=%d bytes, airtime=%lu ms)\n", 
                     queuedPacket.packet.type, packetSize, airTime);
        
        // Remove successfully transmitted packet from queue
        transmissionQueue.pop();
    } else {
        Serial.printf("LoRa Bridge: Transmission failed, code %d (retry %d)\n", 
                     state, queuedPacket.retryCount + 1);
        
        // Schedule retry with exponential backoff
        unsigned long backoff = getRandomBackoff(queuedPacket.retryCount);
        queuedPacket.nextAttempt = now + backoff;
        queuedPacket.retryCount++;
    }
    
    // Restart receiving
    startReceive();
}

bool LoRaBridge::isChannelClear() {
    if (!initialized) {
        return false;
    }
    
    // Perform Channel Activity Detection (CAD)
    int state = radio.scanChannel();
    
    // RadioLib returns RADIOLIB_ERR_NONE if channel is free
    // Returns RADIOLIB_CHANNEL_ACTIVITY_DETECTED if busy
    bool channelClear = (state == RADIOLIB_ERR_NONE);
    
    if (!channelClear) {
        Serial.println("LoRa Bridge: CAD detected channel activity");
    }
    
    return channelClear;
}

unsigned long LoRaBridge::calculateAirTime(size_t packetSize) {
    // Simplified airtime calculation for SF9, BW125, CR4/5
    // This is an approximation - real calculation is more complex
    
    // Symbol time = (2^SF) / BW = (2^9) / 125000 = 4.096 ms
    float symbolTime = 4.096;
    
    // Preamble: 8 symbols + 4.25 symbols
    float preambleTime = (8 + 4.25) * symbolTime;
    
    // Payload symbols (simplified calculation)
    // Real calculation involves more complex formula
    int payloadSymbols = (int)((packetSize * 8.0 + 28 + 16) / (4 * (9 - 2))) + 1;
    if (payloadSymbols < 0) payloadSymbols = 0;
    
    float payloadTime = payloadSymbols * symbolTime;
    
    unsigned long totalTime = (unsigned long)(preambleTime + payloadTime);
    
    // Add safety margin
    return totalTime + 10;
}

bool LoRaBridge::isDutyCycleExceeded(unsigned long airTimeMs) {
    unsigned long now = millis();
    
    // Reset duty cycle window if more than 1 hour has passed
    if (now - dutyCycleStartTime >= DUTY_CYCLE_WINDOW_MS) {
        dutyCycleStartTime = now;
        totalAirTimeMs = 0;
    }
    
    // Check if adding this transmission would exceed the limit
    return (totalAirTimeMs + airTimeMs) > MAX_AIRTIME_MS;
}

void LoRaBridge::updateDutyCycle(unsigned long airTimeMs) {
    totalAirTimeMs += airTimeMs;
    
    // Log duty cycle usage every 10 transmissions
    static int transmissionCount = 0;
    transmissionCount++;
    
    if (transmissionCount % 10 == 0) {
        float dutyCyclePercent = (float)totalAirTimeMs / (float)MAX_AIRTIME_MS * 100.0;
        Serial.printf("LoRa Bridge: Duty cycle usage: %.1f%% (%lu/%lu ms)\n", 
                     dutyCyclePercent, totalAirTimeMs, MAX_AIRTIME_MS);
    }
}

unsigned long LoRaBridge::getRandomBackoff(uint8_t retryCount) {
    // Exponential backoff: base_time * (2^retryCount) + random
    unsigned long baseTime = MIN_BACKOFF_MS;
    unsigned long maxTime = MAX_BACKOFF_MS;
    
    // Calculate exponential backoff
    unsigned long backoffTime = baseTime << retryCount; // baseTime * 2^retryCount
    
    // Cap at maximum backoff time
    if (backoffTime > maxTime) {
        backoffTime = maxTime;
    }
    
    // Add random jitter (0-50% of backoff time) using secure hardware RNG
    unsigned long maxJitter = backoffTime / 2;
    if (maxJitter == 0) maxJitter = 1; // Avoid divide by zero
    
    // Use ESP32 hardware random number generator for cryptographically secure randomness
    unsigned long jitter = esp_random() % maxJitter;
    
    return backoffTime + jitter;
}

// ================ RouteTable Implementation ================

void RouteTable::init() {
    routes.clear();
    pendingRequests.clear();
    nextRequestId = 1;
    Serial.println("RouteTable: Initialized");
}

void RouteTable::addRoute(uint32_t destination, uint32_t nextHop, uint8_t hopCount, uint32_t sequenceNumber) {
    unsigned long now = millis();
    
    auto it = routes.find(destination);
    if (it != routes.end()) {
        // Update existing route if this one is fresher or has better hop count
        RouteEntry& existing = it->second;
        bool isFresher = (sequenceNumber > existing.sequenceNumber);
        bool isBetter = (sequenceNumber == existing.sequenceNumber && hopCount < existing.hopCount);
        
        if (isFresher || isBetter) {
            existing.nextHop = nextHop;
            existing.hopCount = hopCount;
            existing.sequenceNumber = sequenceNumber;
            existing.lastUsed = now;
            existing.expiry = now + ROUTE_TIMEOUT_MS;
            existing.isValid = true;
            
            Serial.printf("RouteTable: Updated route to %08X via %08X (hops=%d, seq=%lu)\n",
                         destination, nextHop, hopCount, sequenceNumber);
        }
    } else {
        // Add new route
        RouteEntry entry(destination, nextHop, hopCount, sequenceNumber);
        routes[destination] = entry;
        
        Serial.printf("RouteTable: Added route to %08X via %08X (hops=%d, seq=%lu)\n",
                     destination, nextHop, hopCount, sequenceNumber);
    }
}

RouteEntry* RouteTable::findRoute(uint32_t destination) {
    auto it = routes.find(destination);
    if (it != routes.end() && it->second.isValid && millis() < it->second.expiry) {
        it->second.lastUsed = millis(); // Update last used time
        return &it->second;
    }
    return nullptr;
}

bool RouteTable::removeRoute(uint32_t destination) {
    auto it = routes.find(destination);
    if (it != routes.end()) {
        Serial.printf("RouteTable: Removed route to %08X\n", destination);
        routes.erase(it);
        return true;
    }
    return false;
}

void RouteTable::cleanupExpiredRoutes() {
    unsigned long now = millis();
    auto it = routes.begin();
    
    int cleanedCount = 0;
    while (it != routes.end()) {
        if (now > it->second.expiry || !it->second.isValid) {
            Serial.printf("RouteTable: Removing expired route to %08X\n", it->second.destination);
            it = routes.erase(it);
            cleanedCount++;
        } else {
            ++it;
        }
    }
    
    if (cleanedCount > 0) {
        Serial.printf("RouteTable: Cleaned up %d expired routes\n", cleanedCount);
    }
}

void RouteTable::printRouteTable() {
    Serial.printf("=== Route Table (%d routes) ===\n", routes.size());
    Serial.println("Destination  NextHop      Hops SeqNum     LastUsed  Expires   Valid");
    Serial.println("------------ ------------ ---- ---------- --------- --------- -----");
    
    unsigned long now = millis();
    for (const auto& pair : routes) {
        const RouteEntry& entry = pair.second;
        unsigned long ageMs = now - entry.lastUsed;
        unsigned long expiresMs = (entry.expiry > now) ? (entry.expiry - now) : 0;
        
        Serial.printf("%08X     %08X     %4d %10lu %7lums %7lums %s\n",
                     entry.destination, entry.nextHop, entry.hopCount, entry.sequenceNumber,
                     ageMs, expiresMs, entry.isValid ? "Yes" : "No");
    }
    Serial.println("===========================");
}

size_t RouteTable::getRouteCount() {
    return routes.size();
}

bool RouteTable::isRouteRequestPending(uint32_t destination) {
    // Check if we have any pending requests for this destination
    for (const auto& pair : pendingRequests) {
        if (pair.second.destination == destination && !pair.second.replied) {
            return true;
        }
    }
    return false;
}

void RouteTable::addRouteRequest(uint32_t requestId, uint32_t originator, uint32_t destination) {
    RouteRequestEntry entry(requestId, originator, destination);
    pendingRequests[requestId] = entry;
    
    Serial.printf("RouteTable: Added route request %lu from %08X to %08X\n",
                 requestId, originator, destination);
}

RouteRequestEntry* RouteTable::findRouteRequest(uint32_t requestId) {
    auto it = pendingRequests.find(requestId);
    return (it != pendingRequests.end()) ? &it->second : nullptr;
}

void RouteTable::cleanupExpiredRequests() {
    unsigned long now = millis();
    auto it = pendingRequests.begin();
    
    int cleanedCount = 0;
    while (it != pendingRequests.end()) {
        if (now - it->second.timestamp > REQUEST_TIMEOUT_MS) {
            Serial.printf("RouteTable: Removing expired request %lu\n", it->second.requestId);
            it = pendingRequests.erase(it);
            cleanedCount++;
        } else {
            ++it;
        }
    }
    
    if (cleanedCount > 0) {
        Serial.printf("RouteTable: Cleaned up %d expired requests\n", cleanedCount);
    }
}

uint32_t RouteTable::generateRequestId() {
    return nextRequestId++;
}

RouteEntry* RouteTable::findBestRoute(uint32_t destination) {
    auto it = routes.find(destination);
    if (it != routes.end() && it->second.isValid) {
        // Update route metrics before returning
        RouteEntry& route = it->second;
        route.routeQuality = calculateRouteScore(route);
        route.loadFactor = calculateLoadFactor(route.nextHop);
        return &route;
    }
    return nullptr;
}

void RouteTable::updateRouteMetrics() {
    for (auto& pair : routes) {
        RouteEntry& route = pair.second;
        if (route.isValid) {
            route.routeQuality = calculateRouteScore(route);
            route.loadFactor = calculateLoadFactor(route.nextHop);
        }
    }
}

uint8_t RouteTable::calculateRouteScore(const RouteEntry& route) {
    // Combined score based on:
    // - Hop count (lower is better)
    // - Link quality to next hop
    // - Load factor of next hop
    // - Power awareness (prefer mains-powered nodes)
    
    uint8_t hopScore = 100 - (route.hopCount * 15); // Each hop reduces score by 15
    if (hopScore < 0) hopScore = 0;
    
    NeighborEntry* neighbor = NeighborTable::getNeighbor(route.nextHop);
    uint8_t linkScore = neighbor ? neighbor->linkQuality : 50; // Default if no data
    
    uint8_t loadScore = 100 - route.loadFactor; // Lower load = higher score
    
    uint8_t powerScore = isPowerAwareRoute(route.nextHop) ? 80 : 100; // Slight penalty for battery
    
    // Weighted average: hop count 30%, link quality 40%, load 20%, power 10%
    uint8_t combinedScore = (hopScore * 30 + linkScore * 40 + loadScore * 20 + powerScore * 10) / 100;
    
    return combinedScore;
}

uint8_t RouteTable::calculateLoadFactor(uint32_t nextHop) {
    NeighborEntry* neighbor = NeighborTable::getNeighbor(nextHop);
    if (neighbor) {
        // Calculate load based on queue depth and connected devices
        uint8_t queueLoad = (neighbor->queueDepth * 100) / 20; // Assume max queue of 20
        uint8_t deviceLoad = (neighbor->connectedDevices * 100) / 10; // Assume max 10 devices
        
        if (queueLoad > 100) queueLoad = 100;
        if (deviceLoad > 100) deviceLoad = 100;
        
        // Combined load factor (queue 70%, devices 30%)
        return (queueLoad * 70 + deviceLoad * 30) / 100;
    }
    return 50; // Default medium load if no data
}

bool RouteTable::isPowerAwareRoute(uint32_t nextHop) {
    NeighborEntry* neighbor = NeighborTable::getNeighbor(nextHop);
    if (neighbor) {
        // Return true if next hop is battery powered (should be avoided for routing)
        return (neighbor->capabilities & NEIGHBOR_CAP_BATTERY_POWERED) != 0;
    }
    return false; // Assume mains powered if unknown
}

// ================ LoRaBridge Mesh Routing Implementation ================

bool LoRaBridge::routeDataPacket(const LoRaPacket& packet) {
    uint32_t myId = getRepeaterID();
    
    // Check for mesh-level duplication (different from BitChat deduplication)
    if (ReliabilityManager::isDuplicatePacket(packet.srcRepeater, packet.seqNum)) {
        Serial.printf("LoRa Bridge: Dropping duplicate data packet from %08X (seq=%d)\n",
                     packet.srcRepeater, packet.seqNum);
        return false;
    }
    
    // Add to seen packets for deduplication
    ReliabilityManager::addSeenPacket(packet.srcRepeater, packet.seqNum);
    
    // Check if packet is for us
    if (packet.destRepeater == myId || packet.destRepeater == LORA_DEST_BROADCAST) {
        // Send ACK if this is a unicast packet (not broadcast)
        if (packet.destRepeater != LORA_DEST_BROADCAST) {
            sendMeshAck(packet.seqNum, packet.srcRepeater);
        }
        
        // Extract BitChat packet and forward to BLE or process locally
        Serial.printf("LoRa Bridge: Received data packet for us (dest=%08X)\n", packet.destRepeater);
        
        // Extract embedded BitChat packet from LoRa data packet payload
        if (packet.payloadLen > 0 && packet.payload != nullptr) {
            BitchatPacket bitchatPacket;
            ParseResult parseResult = parsePacket(packet.payload, packet.payloadLen, bitchatPacket);
            
            if (parseResult == PARSE_SUCCESS) {
                Serial.printf("LoRa Bridge: Successfully extracted BitChat packet (type=0x%02X, TTL=%d)\n", 
                             bitchatPacket.type, bitchatPacket.ttl);
                
                // Forward to MessageRouter for BLE transmission to connected iOS devices
                MessageRouter::forwardToBLE(bitchatPacket);
                
                Serial.printf("LoRa Bridge: BitChat packet forwarded to BLE mesh (%d bytes)\n", 
                             packet.payloadLen);
            } else {
                Serial.printf("LoRa Bridge: Failed to parse embedded BitChat packet, error=%d\n", parseResult);
            }
        } else {
            Serial.println("LoRa Bridge: LoRa data packet has no payload to extract");
        }
        return true;
    }
    
    // Check TTL (hopCount vs maxHops)
    if (packet.hopCount >= packet.maxHops) {
        Serial.printf("LoRa Bridge: Dropping data packet - TTL exceeded (hops=%d, max=%d)\n",
                     packet.hopCount, packet.maxHops);
        return false;
    }
    
    // Look up best route to destination (with load balancing and power awareness)
    RouteEntry* route = RouteTable::findBestRoute(packet.destRepeater);
    
    if (route) {
        // Forward packet using optimal route with reliable delivery
        LoRaPacket forwardPacket = packet;
        forwardPacket.srcRepeater = packet.srcRepeater; // Keep original source
        forwardPacket.nextHop = route->nextHop;
        forwardPacket.hopCount = packet.hopCount + 1;
        
        Serial.printf("LoRa Bridge: Forwarding data packet to %08X via %08X (hop %d, quality=%d%%, load=%d%%)\n",
                     packet.destRepeater, route->nextHop, forwardPacket.hopCount, 
                     route->routeQuality, route->loadFactor);
        
        // Use reliable delivery for unicast packets with adaptive transmission
        if (packet.destRepeater != LORA_DEST_BROADCAST) {
            return sendReliablePacket(forwardPacket, route->nextHop);
        } else {
            // Use adaptive transmission for broadcast packets too
            return transmitAdaptivePacket(forwardPacket, route->nextHop);
        }
    } else {
        // No route found, initiate route discovery
        Serial.printf("LoRa Bridge: No route to %08X, initiating discovery\n", packet.destRepeater);
        initiateRouteDiscovery(packet.destRepeater);
        
        // For now, drop the packet (in a real implementation, we'd queue it)
        Serial.println("LoRa Bridge: Dropping data packet - no route available");
        return false;
    }
}

void LoRaBridge::initiateRouteDiscovery(uint32_t destination) {
    // Check if we already have a pending request for this destination
    if (RouteTable::isRouteRequestPending(destination)) {
        Serial.printf("LoRa Bridge: Route discovery already pending for %08X\n", destination);
        return;
    }
    
    sendRouteRequest(destination);
}

void LoRaBridge::sendRouteRequest(uint32_t destination) {
    uint32_t myId = getRepeaterID();
    uint32_t requestId = RouteTable::generateRequestId();
    
    // Add to pending requests
    RouteTable::addRouteRequest(requestId, myId, destination);
    
    // Create ROUTE_REQUEST packet
    LoRaPacket packet;
    packet.type = LORA_PKT_ROUTE_REQUEST;
    packet.srcRepeater = myId;
    packet.destRepeater = LORA_DEST_BROADCAST;
    packet.nextHop = LORA_DEST_BROADCAST;
    packet.hopCount = 0;
    packet.maxHops = 5; // Limit route discovery flooding
    packet.seqNum = ownSequenceNumber++;
    
    // Create route request payload
    struct __attribute__((packed)) RouteRequestPayload {
        uint32_t requestId;
        uint32_t originator;
        uint32_t destination;
        uint32_t sequenceNumber;
        uint8_t hopCount;
    } payload;
    
    payload.requestId = requestId;
    payload.originator = myId;
    payload.destination = destination;
    payload.sequenceNumber = ownSequenceNumber;
    payload.hopCount = 0;
    
    packet.payloadLen = sizeof(payload);
    memcpy(packet.payload, &payload, sizeof(payload));
    
    Serial.printf("LoRa Bridge: Sending ROUTE_REQUEST for %08X (reqId=%lu)\n", destination, requestId);
    queueLoRaPacket(packet);
}

void LoRaBridge::handleRouteRequest(const LoRaPacket& packet, int rssi) {
    if (packet.payloadLen < 17) { // Minimum size for route request
        Serial.println("LoRa Bridge: Invalid ROUTE_REQUEST - payload too small");
        return;
    }
    
    struct __attribute__((packed)) RouteRequestPayload {
        uint32_t requestId;
        uint32_t originator;
        uint32_t destination;
        uint32_t sequenceNumber;
        uint8_t hopCount;
    };
    
    const RouteRequestPayload* payload = 
        reinterpret_cast<const RouteRequestPayload*>(packet.payload);
    
    uint32_t myId = getRepeaterID();
    
    // Don't process our own requests
    if (payload->originator == myId) {
        return;
    }
    
    // Check if we've already seen this request
    RouteRequestEntry* existingRequest = RouteTable::findRouteRequest(payload->requestId);
    if (existingRequest && existingRequest->replied) {
        Serial.printf("LoRa Bridge: Already replied to request %lu, ignoring\n", payload->requestId);
        return;
    }
    
    // Add reverse route to originator
    RouteTable::addRoute(payload->originator, packet.srcRepeater, payload->hopCount + 1, payload->sequenceNumber);
    
    // Check if we are the destination
    if (payload->destination == myId) {
        // Send ROUTE_REPLY back to originator
        Serial.printf("LoRa Bridge: We are destination for request %lu, sending reply\n", payload->requestId);
        sendRouteReply(payload->destination, payload->originator, payload->requestId, 0);
        
        // Mark request as replied
        if (existingRequest) {
            existingRequest->replied = true;
        } else {
            RouteTable::addRouteRequest(payload->requestId, payload->originator, payload->destination);
            RouteRequestEntry* newRequest = RouteTable::findRouteRequest(payload->requestId);
            if (newRequest) {
                newRequest->replied = true;
            }
        }
        return;
    }
    
    // Check if we have a route to the destination
    RouteEntry* route = RouteTable::findRoute(payload->destination);
    if (route) {
        // Send ROUTE_REPLY back to originator
        Serial.printf("LoRa Bridge: Have route to %08X, sending reply (reqId=%lu)\n", 
                     payload->destination, payload->requestId);
        sendRouteReply(payload->destination, payload->originator, payload->requestId, route->hopCount);
        
        // Mark request as replied
        if (existingRequest) {
            existingRequest->replied = true;
        } else {
            RouteTable::addRouteRequest(payload->requestId, payload->originator, payload->destination);
            RouteRequestEntry* newRequest = RouteTable::findRouteRequest(payload->requestId);
            if (newRequest) {
                newRequest->replied = true;
            }
        }
        return;
    }
    
    // Forward the request if TTL allows
    if (packet.hopCount < packet.maxHops) {
        LoRaPacket forwardPacket = packet;
        forwardPacket.srcRepeater = myId;
        forwardPacket.hopCount = packet.hopCount + 1;
        
        // Update payload hop count
        RouteRequestPayload forwardPayload = *payload;
        forwardPayload.hopCount = packet.hopCount + 1;
        memcpy(forwardPacket.payload, &forwardPayload, sizeof(forwardPayload));
        
        Serial.printf("LoRa Bridge: Forwarding ROUTE_REQUEST for %08X (reqId=%lu, hop=%d)\n",
                     payload->destination, payload->requestId, forwardPacket.hopCount);
        queueLoRaPacket(forwardPacket);
        
        // Track that we've seen this request (but haven't replied)
        if (!existingRequest) {
            RouteTable::addRouteRequest(payload->requestId, payload->originator, payload->destination);
        }
    } else {
        Serial.printf("LoRa Bridge: Dropping ROUTE_REQUEST - TTL exceeded (hop=%d, max=%d)\n",
                     packet.hopCount, packet.maxHops);
    }
}

void LoRaBridge::sendRouteReply(uint32_t destination, uint32_t originator, uint32_t requestId, uint8_t hopCount) {
    uint32_t myId = getRepeaterID();
    
    // Look up route back to originator
    RouteEntry* route = RouteTable::findRoute(originator);
    if (!route) {
        Serial.printf("LoRa Bridge: No route back to originator %08X for ROUTE_REPLY\n", originator);
        return;
    }
    
    // Create ROUTE_REPLY packet
    LoRaPacket packet;
    packet.type = LORA_PKT_ROUTE_REPLY;
    packet.srcRepeater = myId;
    packet.destRepeater = originator;
    packet.nextHop = route->nextHop;
    packet.hopCount = 0;
    packet.maxHops = 10; // Allow longer path for replies
    packet.seqNum = ownSequenceNumber++;
    
    // Create route reply payload
    struct __attribute__((packed)) RouteReplyPayload {
        uint32_t requestId;
        uint32_t originator;
        uint32_t destination;
        uint32_t sequenceNumber;
        uint8_t hopCount;
    } payload;
    
    payload.requestId = requestId;
    payload.originator = originator;
    payload.destination = destination;
    payload.sequenceNumber = ownSequenceNumber;
    payload.hopCount = hopCount;
    
    packet.payloadLen = sizeof(payload);
    memcpy(packet.payload, &payload, sizeof(payload));
    
    Serial.printf("LoRa Bridge: Sending ROUTE_REPLY to %08X for dest %08X (reqId=%lu, hops=%d)\n",
                 originator, destination, requestId, hopCount);
    queueLoRaPacket(packet);
}

void LoRaBridge::handleRouteReply(const LoRaPacket& packet, int rssi) {
    if (packet.payloadLen < 17) { // Minimum size for route reply
        Serial.println("LoRa Bridge: Invalid ROUTE_REPLY - payload too small");
        return;
    }
    
    struct __attribute__((packed)) RouteReplyPayload {
        uint32_t requestId;
        uint32_t originator;
        uint32_t destination;
        uint32_t sequenceNumber;
        uint8_t hopCount;
    };
    
    const RouteReplyPayload* payload = 
        reinterpret_cast<const RouteReplyPayload*>(packet.payload);
    
    uint32_t myId = getRepeaterID();
    
    // Add reverse route to the packet source
    RouteTable::addRoute(packet.srcRepeater, packet.srcRepeater, 1, packet.seqNum);
    
    // Check if this reply is for us
    if (payload->originator == myId) {
        // This is a reply to our route request
        Serial.printf("LoRa Bridge: Received ROUTE_REPLY for our request %lu to %08X (hops=%d)\n",
                     payload->requestId, payload->destination, payload->hopCount + packet.hopCount);
        
        // Add route to destination
        RouteTable::addRoute(payload->destination, packet.srcRepeater, 
                           payload->hopCount + packet.hopCount + 1, payload->sequenceNumber);
        return;
    }
    
    // Check if packet is destined for us
    if (packet.destRepeater == myId) {
        Serial.printf("LoRa Bridge: Received ROUTE_REPLY destined for us but not our request - dropping\n");
        return;
    }
    
    // Forward the reply toward its destination
    RouteEntry* route = RouteTable::findRoute(packet.destRepeater);
    if (route && packet.hopCount < packet.maxHops) {
        LoRaPacket forwardPacket = packet;
        forwardPacket.srcRepeater = myId;
        forwardPacket.nextHop = route->nextHop;
        forwardPacket.hopCount = packet.hopCount + 1;
        
        // Update payload hop count
        RouteReplyPayload forwardPayload = *payload;
        forwardPayload.hopCount = payload->hopCount + packet.hopCount + 1;
        memcpy(forwardPacket.payload, &forwardPayload, sizeof(forwardPayload));
        
        Serial.printf("LoRa Bridge: Forwarding ROUTE_REPLY to %08X via %08X (hop=%d)\n",
                     packet.destRepeater, route->nextHop, forwardPacket.hopCount);
        queueLoRaPacket(forwardPacket);
    } else {
        Serial.printf("LoRa Bridge: Cannot forward ROUTE_REPLY - no route to %08X\n", packet.destRepeater);
    }
}

// ================ ReliabilityManager Implementation ================

void ReliabilityManager::init() {
    pendingAcks.clear();
    meshDedupeCache.clear();
    alternativeRoutes.clear();
    nextPacketId = 1;
    Serial.println("ReliabilityManager: Initialized");
}

uint32_t ReliabilityManager::generatePacketId() {
    return nextPacketId++;
}

void ReliabilityManager::addPendingAck(uint32_t packetId, uint32_t destination, const LoRaPacket& packet) {
    PendingAckEntry entry(packetId, destination, packet);
    pendingAcks[packetId] = entry;
    
    Serial.printf("ReliabilityManager: Added pending ACK for packet %lu to %08X\n", packetId, destination);
}

void ReliabilityManager::handleMeshAck(uint32_t packetId, uint32_t source) {
    auto it = pendingAcks.find(packetId);
    if (it != pendingAcks.end()) {
        Serial.printf("ReliabilityManager: Received ACK for packet %lu from %08X\n", packetId, source);
        pendingAcks.erase(it);
    } else {
        Serial.printf("ReliabilityManager: Received ACK for unknown packet %lu from %08X\n", packetId, source);
    }
}

void ReliabilityManager::processAckTimeouts() {
    unsigned long now = millis();
    auto it = pendingAcks.begin();
    
    while (it != pendingAcks.end()) {
        PendingAckEntry& entry = it->second;
        
        if (now >= entry.nextRetry) {
            if (entry.retryCount >= MAX_RETRIES) {
                // Mark route as failed and try alternative
                Serial.printf("ReliabilityManager: Packet %lu failed after %d retries, marking route failed\n",
                             entry.packetId, MAX_RETRIES);
                
                markRouteFailed(entry.originalPacket.destRepeater, entry.destination);
                
                // Try alternative route
                AlternativeRoute* altRoute = getAlternativeRoute(entry.originalPacket.destRepeater, entry.destination);
                if (altRoute) {
                    Serial.printf("ReliabilityManager: Trying alternative route via %08X\n", altRoute->nextHop);
                    entry.destination = altRoute->nextHop;
                    entry.retryCount = 0;
                    entry.nextRetry = now + ACK_TIMEOUT_MS;
                    entry.originalPacket.nextHop = altRoute->nextHop;
                    
                    // Queue packet with new route
                    LoRaBridge::queueLoRaPacket(entry.originalPacket);
                    ++it;
                } else {
                    Serial.printf("ReliabilityManager: No alternative route available, dropping packet %lu\n", entry.packetId);
                    it = pendingAcks.erase(it);
                }
            } else {
                // Retry transmission
                entry.retryCount++;
                entry.nextRetry = now + (ACK_TIMEOUT_MS * (1 << entry.retryCount)); // Exponential backoff
                
                Serial.printf("ReliabilityManager: Retrying packet %lu (attempt %d)\n", 
                             entry.packetId, entry.retryCount + 1);
                
                LoRaBridge::queueLoRaPacket(entry.originalPacket);
                ++it;
            }
        } else {
            ++it;
        }
    }
}

void ReliabilityManager::cleanupExpiredEntries() {
    unsigned long now = millis();
    
    // Clean up deduplication cache
    auto dedupeIt = meshDedupeCache.begin();
    int dedupeCleanedCount = 0;
    while (dedupeIt != meshDedupeCache.end()) {
        if (now - dedupeIt->second.timestamp > DEDUPE_TIMEOUT_MS) {
            dedupeIt = meshDedupeCache.erase(dedupeIt);
            dedupeCleanedCount++;
        } else {
            ++dedupeIt;
        }
    }
    
    if (dedupeCleanedCount > 0) {
        Serial.printf("ReliabilityManager: Cleaned up %d expired dedupe entries\n", dedupeCleanedCount);
    }
    
    // Clean up very old pending ACKs (shouldn't happen with proper timeout handling)
    auto ackIt = pendingAcks.begin();
    int ackCleanedCount = 0;
    while (ackIt != pendingAcks.end()) {
        if (now - ackIt->second.sentTime > 30000) { // 30 seconds max
            Serial.printf("ReliabilityManager: Force cleaning very old pending ACK %lu\n", ackIt->second.packetId);
            ackIt = pendingAcks.erase(ackIt);
            ackCleanedCount++;
        } else {
            ++ackIt;
        }
    }
    
    if (ackCleanedCount > 0) {
        Serial.printf("ReliabilityManager: Force cleaned %d very old pending ACKs\n", ackCleanedCount);
    }
}

bool ReliabilityManager::isDuplicatePacket(uint32_t sourceRepeater, uint32_t sequenceNumber) {
    uint64_t key = ((uint64_t)sourceRepeater << 32) | sequenceNumber;
    return meshDedupeCache.find(key) != meshDedupeCache.end();
}

void ReliabilityManager::addSeenPacket(uint32_t sourceRepeater, uint32_t sequenceNumber) {
    uint64_t key = ((uint64_t)sourceRepeater << 32) | sequenceNumber;
    MeshDedupeEntry entry(sourceRepeater, sequenceNumber);
    meshDedupeCache[key] = entry;
}

void ReliabilityManager::cleanupDedupeEntries() {
    // This is called as part of cleanupExpiredEntries()
}

void ReliabilityManager::addAlternativeRoute(uint32_t destination, uint32_t nextHop, uint8_t hopCount, uint32_t sequenceNumber, uint8_t priority) {
    AlternativeRoute route(destination, nextHop, hopCount, sequenceNumber, priority);
    alternativeRoutes[destination].push_back(route);
    
    // Sort routes by priority (lower number = higher priority)
    std::sort(alternativeRoutes[destination].begin(), alternativeRoutes[destination].end(),
              [](const AlternativeRoute& a, const AlternativeRoute& b) {
                  if (a.priority != b.priority) return a.priority < b.priority;
                  return a.hopCount < b.hopCount; // Prefer shorter routes within same priority
              });
    
    Serial.printf("ReliabilityManager: Added alternative route to %08X via %08X (hops=%d, priority=%d)\n",
                 destination, nextHop, hopCount, priority);
}

AlternativeRoute* ReliabilityManager::getAlternativeRoute(uint32_t destination, uint32_t failedNextHop) {
    auto it = alternativeRoutes.find(destination);
    if (it != alternativeRoutes.end()) {
        for (auto& route : it->second) {
            if (route.nextHop != failedNextHop) {
                route.lastUsed = millis();
                return &route;
            }
        }
    }
    return nullptr;
}

void ReliabilityManager::markRouteFailed(uint32_t destination, uint32_t nextHop) {
    // Remove the failed route from main route table
    RouteEntry* mainRoute = RouteTable::findRoute(destination);
    if (mainRoute && mainRoute->nextHop == nextHop) {
        RouteTable::removeRoute(destination);
        Serial.printf("ReliabilityManager: Marked main route to %08X via %08X as failed\n", destination, nextHop);
    }
    
    // Remove failed next hop from alternative routes
    auto it = alternativeRoutes.find(destination);
    if (it != alternativeRoutes.end()) {
        it->second.erase(
            std::remove_if(it->second.begin(), it->second.end(),
                          [nextHop](const AlternativeRoute& route) {
                              return route.nextHop == nextHop;
                          }),
            it->second.end());
        
        if (it->second.empty()) {
            alternativeRoutes.erase(it);
        }
    }
}

// ================ LoRaBridge Reliable Delivery Implementation ================

bool LoRaBridge::sendReliablePacket(const LoRaPacket& packet, uint32_t nextHop) {
    // Generate unique packet ID for acknowledgment tracking
    uint32_t packetId = ReliabilityManager::generatePacketId();
    
    // Create packet with unique ID
    LoRaPacket reliablePacket = packet;
    reliablePacket.seqNum = packetId; // Use seqNum field for packet ID
    
    // Add to pending acknowledgments
    ReliabilityManager::addPendingAck(packetId, nextHop, reliablePacket);
    
    // Queue for transmission
    Serial.printf("LoRa Bridge: Sending reliable packet %lu to %08X\n", packetId, nextHop);
    return queueLoRaPacket(reliablePacket);
}

void LoRaBridge::handleMeshAck(const LoRaPacket& packet, int rssi) {
    if (packet.payloadLen < 4) {
        Serial.println("LoRa Bridge: Invalid MESH_ACK - payload too small");
        return;
    }
    
    // Extract packet ID from payload
    uint32_t packetId;
    memcpy(&packetId, packet.payload, sizeof(packetId));
    
    Serial.printf("LoRa Bridge: Received MESH_ACK for packet %lu from %08X\n", packetId, packet.srcRepeater);
    
    // Forward to reliability manager
    ReliabilityManager::handleMeshAck(packetId, packet.srcRepeater);
}

void LoRaBridge::sendMeshAck(uint32_t packetId, uint32_t destination) {
    uint32_t myId = getRepeaterID();
    
    // Create MESH_ACK packet
    LoRaPacket ackPacket;
    ackPacket.type = LORA_PKT_MESH_ACK;
    ackPacket.srcRepeater = myId;
    ackPacket.destRepeater = destination;
    ackPacket.nextHop = destination; // Direct ACK to sender
    ackPacket.hopCount = 0;
    ackPacket.maxHops = 3; // Keep ACKs short-lived
    ackPacket.seqNum = ownSequenceNumber++;
    
    // Pack packet ID in payload
    ackPacket.payloadLen = sizeof(packetId);
    memcpy(ackPacket.payload, &packetId, sizeof(packetId));
    
    Serial.printf("LoRa Bridge: Sending MESH_ACK for packet %lu to %08X\n", packetId, destination);
    queueLoRaPacket(ackPacket);
}

void LoRaBridge::processReliabilityTimeouts() {
    ReliabilityManager::processAckTimeouts();
}

// ================ LoRaBridge Adaptive Optimization Implementation ================

bool LoRaBridge::transmitAdaptivePacket(const LoRaPacket& packet, uint32_t targetRepeater) {
    if (!adaptiveRatesEnabled) {
        return transmitLoRaPacket(packet);
    }
    
    // Get optimal spreading factor for target
    uint8_t optimalSF = NeighborTable::getOptimalSpreadingFactor(targetRepeater);
    
    // Only change SF if it's different from current and target is not broadcast
    if (optimalSF != currentSpreadingFactor && targetRepeater != LORA_DEST_BROADCAST) {
        Serial.printf("LoRa Bridge: Switching to SF%d for transmission to %08X\n", optimalSF, targetRepeater);
        
        // Temporarily change spreading factor
        uint8_t originalSF = currentSpreadingFactor;
        int state = radio.setSpreadingFactor(optimalSF);
        if (state == RADIOLIB_ERR_NONE) {
            currentSpreadingFactor = optimalSF;
            
            // Transmit with optimal SF
            bool result = transmitLoRaPacket(packet);
            
            // Restore original SF for general use
            radio.setSpreadingFactor(originalSF);
            currentSpreadingFactor = originalSF;
            
            return result;
        } else {
            Serial.printf("LoRa Bridge: Failed to set SF%d, using default SF%d\n", optimalSF, originalSF);
        }
    }
    
    // Fall back to normal transmission
    return transmitLoRaPacket(packet);
}

void LoRaBridge::updateNetworkOptimization() {
    unsigned long now = millis();
    
    // Only update periodically to avoid excessive processing
    if (now - lastOptimizationUpdate < OPTIMIZATION_UPDATE_INTERVAL_MS) {
        return;
    }
    
    lastOptimizationUpdate = now;
    
    // Update adaptive rates in neighbor table
    if (adaptiveRatesEnabled) {
        NeighborTable::updateAdaptiveRates();
    }
    
    // Update route metrics for load balancing
    RouteTable::updateRouteMetrics();
    
    Serial.println("LoRa Bridge: Network optimization update completed");
}

// Rate limiting implementation to prevent DoS attacks
bool LoRaBridge::checkRateLimit(uint32_t sourceId) {
    unsigned long now = millis();
    
    // Get or create rate limit entry for this source
    RateLimitEntry& entry = rateLimitTable[sourceId];
    
    // Check if we need to start a new time window
    if (now - entry.windowStart >= RATE_LIMIT_WINDOW_MS) {
        // Start new window
        entry.windowStart = now;
        entry.packetCount = 0;
    }
    
    // Check if source has exceeded rate limit
    if (entry.packetCount >= MAX_PACKETS_PER_WINDOW) {
        Serial.printf("LoRa Bridge: Rate limit exceeded for source 0x%08X (%d packets in %lu ms)\n", 
                     sourceId, entry.packetCount, RATE_LIMIT_WINDOW_MS);
        return false; // Rate limit exceeded
    }
    
    return true; // Rate limit not exceeded
}

void LoRaBridge::updateRateLimit(uint32_t sourceId) {
    unsigned long now = millis();
    
    // Get or create rate limit entry for this source
    RateLimitEntry& entry = rateLimitTable[sourceId];
    
    // Update packet count and timestamp
    entry.packetCount++;
    entry.lastPacket = now;
    
    // Cleanup old entries periodically (every 10 minutes)
    static unsigned long lastCleanup = 0;
    if (now - lastCleanup > 600000) { // 10 minutes
        auto it = rateLimitTable.begin();
        while (it != rateLimitTable.end()) {
            // Remove entries that haven't been used in the last 10 minutes
            if (now - it->second.lastPacket > 600000) {
                it = rateLimitTable.erase(it);
            } else {
                ++it;
            }
        }
        lastCleanup = now;
    }
}