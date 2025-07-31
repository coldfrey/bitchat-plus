#include "lora_bridge.h"
#include "message_router.h"
#include <Arduino.h>

// Static member definitions
SX1262 LoRaBridge::radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);
bool LoRaBridge::initialized = false;
bool LoRaBridge::receiving = false;
unsigned long LoRaBridge::lastRxCheck = 0;
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
    
    // Periodic status check every 30 seconds
    unsigned long now = millis();
    if (now - lastRxCheck > 30000) {
        lastRxCheck = now;
        Serial.printf("LoRa Bridge: Stats - TX: %lu, RX: %lu, Last RSSI: %d dBm, SNR: %.1f dB\n",
                     txCount, rxCount, lastRSSI, lastSNR);
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
    
    // Parse the received packet
    BitchatPacket packet;
    ParseResult result = parsePacket(buffer, receivedSize, packet);
    
    if (result != PARSE_SUCCESS) {
        Serial.printf("LoRa Bridge: Failed to parse received packet, error %d\n", result);
        return;
    }
    
    // Forward to message router for processing
    MessageRouter::handleLoRaMessage(packet);
}