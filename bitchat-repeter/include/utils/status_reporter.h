#pragma once

#include <Arduino.h>
#include "../core/bitchat_protocol.h"

class StatusReporter {
public:
    static void init();
    static void process();
    
    // Status information
    static void sendStatusUpdate();
    static String getRepeaterNickname();
    static String getFirmwareVersion();
    
    // Statistics
    static uint32_t getTotalMessagesSent();
    static uint32_t getTotalMessagesReceived();
    static unsigned long getUptimeSeconds();
    
private:
    static unsigned long lastStatusUpdate;
    static uint32_t totalMessagesSent;
    static uint32_t totalMessagesReceived;
    static unsigned long bootTime;
    
    static const unsigned long STATUS_UPDATE_INTERVAL_MS = 30000; // 30 seconds
    static const String FIRMWARE_VERSION;
    
    static BitchatPacket createStatusPacket();
    static String formatStatusMessage();
};