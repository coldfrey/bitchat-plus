#include "lora_bridge.h"
#include "message_router.h"
#include <Arduino.h>

// NeighborTable static member definitions
std::map<uint32_t, NeighborEntry> NeighborTable::neighbors;

// RouteTable static member definitions
std::map<uint32_t, RouteEntry> RouteTable::routes;
std::map<uint32_t, RouteRequestEntry> RouteTable::pendingRequests;
uint32_t RouteTable::nextRequestId = 1;

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
int LoRaBridge::lastRSSI = 0;
float LoRaBridge::lastSNR = 0.0;
unsigned long LoRaBridge::txCount = 0;
unsigned long LoRaBridge::rxCount = 0;
volatile bool LoRaBridge::receivedFlag = false;

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

// RouteTable constants
const unsigned long RouteTable::ROUTE_TIMEOUT_MS;
const unsigned long RouteTable::REQUEST_TIMEOUT_MS;

void LoRaBridge::init() {
    Serial.println("LoRa Bridge: Initializing SX1262 radio...");
    
    // Initialize SPI pins
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    
    // Initialize radio
    Serial.printf("LoRa Bridge: CS=%d, DIO1=%d, RST=%d, BUSY=%d\n", 
                 LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
    
    int state = radio.begin();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("LoRa Bridge: Radio initialization failed, code %d\n", state);
        return;
    }
    
    Serial.println("LoRa Bridge: Radio initialized, configuring parameters...");
    
    // Configure radio parameters
    if (!configure()) {
        Serial.println("LoRa Bridge: Configuration failed");
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
    
    // Handle received messages
    if (receivedFlag) {
        receivedFlag = false;
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
    if (!initialized) {
        Serial.println("LoRa Bridge: Cannot queue - radio not initialized");
        return false;
    }
    
    // Check if queue is full
    if (transmissionQueue.size() >= MAX_QUEUE_SIZE) {
        Serial.printf("LoRa Bridge: Queue full (%d packets), dropping oldest\n", transmissionQueue.size());
        transmissionQueue.pop(); // Drop oldest packet
    }
    
    // Add packet to queue
    QueuedPacket queuedPacket(packet);
    transmissionQueue.push(queuedPacket);
    
    Serial.printf("LoRa Bridge: Queued LoRa packet (type=0x%02X, queue size: %d)\n", 
                 packet.type, transmissionQueue.size());
    
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

// Interrupt handler - keep it minimal
void LoRaBridge::onReceive() {
    receivedFlag = true;
}

bool LoRaBridge::configure() {
    int state;
    
    // Set frequency
    state = radio.setFrequency(FREQUENCY);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("LoRa Bridge: Failed to set frequency, code %d\n", state);
        return false;
    }
    
    // Set bandwidth
    state = radio.setBandwidth(BANDWIDTH);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("LoRa Bridge: Failed to set bandwidth, code %d\n", state);
        return false;
    }
    
    // Set spreading factor
    state = radio.setSpreadingFactor(SPREADING_FACTOR);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("LoRa Bridge: Failed to set spreading factor, code %d\n", state);
        return false;
    }
    
    // Set coding rate
    state = radio.setCodingRate(CODING_RATE);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("LoRa Bridge: Failed to set coding rate, code %d\n", state);
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
    // Read the received data
    uint8_t buffer[256];
    int state = radio.readData(buffer, sizeof(buffer));
    
    if (state < 0) {
        Serial.printf("LoRa Bridge: Failed to read received data, code %d\n", state);
        return;
    }
    
    size_t receivedSize = state;
    rxCount++;
    
    // Get RSSI and SNR
    lastRSSI = radio.getRSSI();
    lastSNR = radio.getSNR();
    
    Serial.printf("LoRa Bridge: Received %d bytes (RSSI: %d dBm, SNR: %.1f dB)\n", 
                 receivedSize, lastRSSI, lastSNR);
    
    // Check if this is a LoRa mesh packet (starts with magic 0xBC)
    if (receivedSize >= 3 && buffer[0] == LORA_MAGIC) {
        handleReceivedLoRaPacket(buffer, receivedSize);
        return;
    }
    
    // Otherwise, try to parse as BitChat packet
    BitchatPacket packet;
    ParseResult result = parsePacket(buffer, receivedSize, packet);
    
    if (result != PARSE_SUCCESS) {
        Serial.printf("LoRa Bridge: Failed to parse received packet, error %d\n", result);
        return;
    }
    
    // Forward to message router for processing
    MessageRouter::handleLoRaMessage(packet);
}

void LoRaBridge::handleReceivedLoRaPacket(const uint8_t* buffer, size_t receivedSize) {
    
    // Parse the LoRa packet
    LoRaPacket loraPacket;
    LoRaParseResult result = parseLoRaPacket(buffer, receivedSize, loraPacket);
    
    if (result != LORA_PARSE_SUCCESS) {
        Serial.printf("LoRa Bridge: Failed to parse LoRa packet, error %d\n", result);
        return;
    }
    
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
            Serial.printf("LoRa Bridge: Received MESH_ACK packet - not yet implemented\n");
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
    if (packet.payloadLen < 26) { // Minimum size for announcement
        Serial.println("LoRa Bridge: Invalid neighbor announcement - payload too small");
        return;
    }
    
    struct __attribute__((packed)) NeighborAnnouncement {
        uint32_t repeaterId;
        char name[16];
        uint8_t connectedDevices;
        uint8_t queueDepth;
        uint8_t capabilities;
        uint8_t firmwareVersion;
    };
    
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
    
    // Add random jitter (0-50% of backoff time)
    unsigned long jitter = random(0, backoffTime / 2);
    
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

// ================ LoRaBridge Mesh Routing Implementation ================

bool LoRaBridge::routeDataPacket(const LoRaPacket& packet) {
    uint32_t myId = getRepeaterID();
    
    // Check if packet is for us
    if (packet.destRepeater == myId || packet.destRepeater == LORA_DEST_BROADCAST) {
        // Extract BitChat packet and forward to BLE or process locally
        Serial.printf("LoRa Bridge: Received data packet for us (dest=%08X)\n", packet.destRepeater);
        
        // TODO: Forward BitChat packet payload to MessageRouter for BLE transmission
        // For now, just log the reception
        Serial.printf("LoRa Bridge: Data packet payload (%d bytes) - forwarding not yet implemented\n", 
                     packet.payloadLen);
        return true;
    }
    
    // Check TTL (hopCount vs maxHops)
    if (packet.hopCount >= packet.maxHops) {
        Serial.printf("LoRa Bridge: Dropping data packet - TTL exceeded (hops=%d, max=%d)\n",
                     packet.hopCount, packet.maxHops);
        return false;
    }
    
    // Look up route to destination
    RouteEntry* route = RouteTable::findRoute(packet.destRepeater);
    
    if (route) {
        // Forward packet using known route
        LoRaPacket forwardPacket = packet;
        forwardPacket.srcRepeater = packet.srcRepeater; // Keep original source
        forwardPacket.nextHop = route->nextHop;
        forwardPacket.hopCount = packet.hopCount + 1;
        
        Serial.printf("LoRa Bridge: Forwarding data packet to %08X via %08X (hop %d)\n",
                     packet.destRepeater, route->nextHop, forwardPacket.hopCount);
        
        return queueLoRaPacket(forwardPacket);
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