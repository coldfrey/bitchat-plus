#include "lora_bridge.h"
#include "message_router.h"
#include <Arduino.h>

// NeighborTable static member definitions
std::map<uint32_t, NeighborEntry> NeighborTable::neighbors;

// LoRaBridge static member definitions
SX1262 LoRaBridge::radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
bool LoRaBridge::initialized = false;
bool LoRaBridge::receiving = false;
unsigned long LoRaBridge::lastRxCheck = 0;
unsigned long LoRaBridge::lastNeighborAnnouncement = 0;
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
    if (!initialized) {
        Serial.println("LoRa Bridge: Cannot transmit - radio not initialized");
        return false;
    }
    
    // Serialize LoRa packet to binary data
    uint8_t buffer[256];
    size_t packetSize = serializeLoRaPacket(packet, buffer, sizeof(buffer));
    
    if (packetSize == 0) {
        Serial.println("LoRa Bridge: Failed to serialize LoRa packet for transmission");
        return false;
    }
    
    // Temporarily stop receiving
    receiving = false;
    
    // Transmit the packet
    int state = radio.transmit(buffer, packetSize);
    
    bool success = (state == RADIOLIB_ERR_NONE);
    if (success) {
        txCount++;
        Serial.printf("LoRa Bridge: Transmitted LoRa packet (type=0x%02X, size=%d bytes)\n", 
                     packet.type, packetSize);
    } else {
        Serial.printf("LoRa Bridge: LoRa transmission failed, code %d\n", state);
    }
    
    // Restart receiving
    startReceive();
    
    return success;
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
            // Data packet contains a BitChat packet in the payload
            Serial.println("LoRa Bridge: Received DATA packet - not yet implemented");
            break;
            
        case LORA_PKT_ROUTE_REQUEST:
        case LORA_PKT_ROUTE_REPLY:
        case LORA_PKT_MESH_ACK:
            Serial.printf("LoRa Bridge: Received packet type 0x%02X - not yet implemented\n", loraPacket.type);
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