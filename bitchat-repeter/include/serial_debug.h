#pragma once

#include <Arduino.h>

class SerialDebug {
public:
    static void init();
    static void process();
    
    // Command handlers
    static void handleCommand(const String& command);
    static void printHelp();
    
    // Individual command implementations
    static void showStatus();
    static void showStats();
    static void showMesh();
    static void pingRepeater(uint32_t repeaterId);
    static void traceRoute(uint32_t repeaterId);
    static void resetDevice();
    static void showConfig();
    static void showLoRaStats();
    static void showBLEStats();
    static void setLogLevel(uint8_t level);
    
    // Utility functions
    static void printTable(const String& title, const String headers[], const String data[][10], int rows, int cols);
    static void printSeparator(int width = 80);
    static String formatUptime(unsigned long seconds);
    static String formatBytes(size_t bytes);
    
private:
    static String commandBuffer;
    static bool debugEnabled;
    static unsigned long lastStatsTime;
    
    // Statistics tracking
    static unsigned long messagesPerMinute;
    static unsigned long lastMessageCount;
    static unsigned long statsStartTime;
    
    // Command parsing
    static void parseCommand(const String& input);
    static uint32_t parseHexId(const String& hexStr);
    static bool isValidHexId(const String& hexStr);
};