#pragma once

#include <RadioLib.h>
#include "hardware_config.h"
#include "bitchat_protocol.h"

class LoRaBridge {
public:
    static void init();
    static void process();
    
    // Message transmission
    static bool transmit(const BitchatPacket& packet);
    static void startReceive();
    
    // Status and statistics
    static bool isReceiving();
    static int getLastRSSI();
    static float getLastSNR();
    
private:
    // RadioLib SX1262 instance
    static SX1262 radio;
    
    // Radio state
    static bool initialized;
    static bool receiving;
    static unsigned long lastRxCheck;
    
    // Statistics
    static int lastRSSI;
    static float lastSNR;
    static unsigned long txCount;
    static unsigned long rxCount;
    
    // Configuration constants
    static const float FREQUENCY;
    static const float BANDWIDTH;
    static const uint8_t SPREADING_FACTOR;
    static const uint8_t CODING_RATE;
    static const int8_t TX_POWER;
    static const uint8_t SYNC_WORD;
    
    // Interrupt handling
    static volatile bool receivedFlag;
    static void onReceive();
    
    // Helper functions
    static bool configure();
    static void handleReceivedMessage();
};