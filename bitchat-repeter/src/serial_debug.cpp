#include "serial_debug.h"
#include "ble_gateway.h"
#include "lora_bridge.h"
#include "message_router.h"
#include "config_manager.h"
#include "connection_manager.h"
#include "message_priority_manager.h"
#include "power_manager.h"
#include "status_reporter.h"
#include "loop_prevention_test.h"
#include <Arduino.h>
#include <WiFi.h>

// Static member definitions
String SerialDebug::commandBuffer = "";
bool SerialDebug::debugEnabled = false;
unsigned long SerialDebug::lastStatsTime = 0;
unsigned long SerialDebug::messagesPerMinute = 0;
unsigned long SerialDebug::lastMessageCount = 0;
unsigned long SerialDebug::statsStartTime = 0;

void SerialDebug::init() {
    debugEnabled = true;
    statsStartTime = millis();
    lastStatsTime = statsStartTime;
    
    Serial.println();
    printSeparator();
    Serial.println("  BitChat Repeater Debug Interface");
    Serial.println("  Type 'help' for available commands");
    printSeparator();
    Serial.print("debug> ");
}

void SerialDebug::process() {
    if (!debugEnabled) return;
    
    // Read serial input
    while (Serial.available()) {
        char c = Serial.read();
        
        if (c == '\n' || c == '\r') {
            if (commandBuffer.length() > 0) {
                Serial.println(); // Echo newline
                handleCommand(commandBuffer);
                commandBuffer = "";
                Serial.print("debug> ");
            }
        } else if (c == '\b' || c == 127) { // Backspace
            if (commandBuffer.length() > 0) {
                commandBuffer.remove(commandBuffer.length() - 1);
                Serial.print("\b \b"); // Erase character on screen
            }
        } else if (c >= 32 && c <= 126) { // Printable characters
            commandBuffer += c;
            Serial.print(c); // Echo character
        }
    }
    
    // Update rolling statistics every minute
    unsigned long now = millis();
    if (now - lastStatsTime >= 60000) { // Every minute
        unsigned long currentMessages = StatusReporter::getTotalMessagesSent() + StatusReporter::getTotalMessagesReceived();
        messagesPerMinute = currentMessages - lastMessageCount;
        lastMessageCount = currentMessages;
        lastStatsTime = now;
    }
}

void SerialDebug::handleCommand(const String& command) {
    String cmd = command;
    cmd.trim();
    cmd.toLowerCase();
    
    if (cmd == "help" || cmd == "?") {
        printHelp();
    } else if (cmd == "status") {
        showStatus();
    } else if (cmd == "stats") {
        showStats();
    } else if (cmd == "mesh") {
        showMesh();
    } else if (cmd.startsWith("ping ")) {
        String idStr = cmd.substring(5);
        uint32_t repeaterId = parseHexId(idStr);
        if (repeaterId != 0) {
            pingRepeater(repeaterId);
        } else {
            Serial.println("ERROR: Invalid repeater ID format. Use hex format like: 12345678");
        }
    } else if (cmd.startsWith("trace ")) {
        String idStr = cmd.substring(6);
        uint32_t repeaterId = parseHexId(idStr);
        if (repeaterId != 0) {
            traceRoute(repeaterId);
        } else {
            Serial.println("ERROR: Invalid repeater ID format. Use hex format like: 12345678");
        }
    } else if (cmd == "reset") {
        resetDevice();
    } else if (cmd == "config") {
        showConfig();
    } else if (cmd == "lora") {
        showLoRaStats();
    } else if (cmd == "ble") {
        showBLEStats();
    } else if (cmd.startsWith("log ")) {
        String levelStr = cmd.substring(4);
        uint8_t level = levelStr.toInt();
        if (level <= 3) {
            setLogLevel(level);
        } else {
            Serial.println("ERROR: Log level must be 0-3");
        }
    } else if (cmd == "looptest") {
        LoopPreventionTest::showTestStatus();
    } else if (cmd.startsWith("looptest start")) {
        // Parse optional parameters: looptest start [ttl] [duration_seconds]
        String params = cmd.substring(15);
        params.trim();
        
        uint8_t ttl = 5;
        unsigned long duration = 300; // 5 minutes default
        
        if (params.length() > 0) {
            int spaceIndex = params.indexOf(' ');
            if (spaceIndex > 0) {
                ttl = params.substring(0, spaceIndex).toInt();
                duration = params.substring(spaceIndex + 1).toInt();
            } else {
                ttl = params.toInt();
            }
        }
        
        if (ttl < 1 || ttl > 10) {
            Serial.println("ERROR: TTL must be 1-10");
        } else if (duration < 30 || duration > 3600) {
            Serial.println("ERROR: Duration must be 30-3600 seconds");
        } else {
            LoopPreventionTest::startTestCommand(ttl, duration);
        }
    } else if (cmd == "looptest stop") {
        LoopPreventionTest::stopTestCommand();
    } else if (cmd == "looptest results") {
        LoopPreventionTest::printTestResults();
    } else if (cmd == "looptest detailed") {
        LoopPreventionTest::printDetailedAnalysis();
    } else if (cmd == "") {
        // Empty command, just show prompt
    } else {
        Serial.printf("Unknown command: '%s'. Type 'help' for available commands.\n", cmd.c_str());
    }
}

void SerialDebug::printHelp() {
    printSeparator();
    Serial.println("Available Commands:");
    Serial.println("  status       - Show connections, queues, uptime");
    Serial.println("  stats        - Message counts, dedup hits, errors");
    Serial.println("  mesh         - Show neighbor and route tables");
    Serial.println("  ping <id>    - Test mesh connectivity to repeater");
    Serial.println("  trace <id>   - Show route path to repeater");
    Serial.println("  reset        - Restart device");
    Serial.println("  config       - Show current configuration");
    Serial.println("  lora         - LoRa radio statistics");
    Serial.println("  ble          - BLE connection details");
    Serial.println("  log <level>  - Set debug level (0-3)");
    Serial.println("  looptest     - Show loop prevention test status");
    Serial.println("  looptest start [ttl] [duration] - Start loop test");
    Serial.println("  looptest stop     - Stop current loop test");
    Serial.println("  looptest results  - Show test results");
    Serial.println("  looptest detailed - Show detailed analysis");
    Serial.println("  help/?       - Show this help message");
    printSeparator();
    Serial.println("Repeater ID format: 8-digit hex (e.g., 12345678)");
    Serial.println("Loop test example: looptest start 5 300 (TTL=5, 5min test)");
}

void SerialDebug::showStatus() {
    printSeparator();
    Serial.println("SYSTEM STATUS");
    printSeparator();
    
    // Basic system info
    Serial.printf("Device Name:     %s\n", ConfigManager::getDeviceName().c_str());
    Serial.printf("Gateway ID:      %s\n", BLEGateway::getGatewayID().c_str());
    Serial.printf("Firmware:        %s\n", StatusReporter::getFirmwareVersion().c_str());
    Serial.printf("Uptime:          %s\n", formatUptime(StatusReporter::getUptimeSeconds()).c_str());
    
    // Memory info
    Serial.printf("Free Heap:       %s\n", formatBytes(ESP.getFreeHeap()).c_str());
    Serial.printf("Heap Size:       %s\n", formatBytes(ESP.getHeapSize()).c_str());
    
    // Connection status
    Serial.printf("iOS Devices:     %d\n", BLEGateway::getConnectedDeviceCount());
    Serial.printf("Mesh Neighbors:  %d\n", NeighborTable::getNeighborCount());
    Serial.printf("Active Routes:   %d\n", RouteTable::getRouteCount());
    
    // Queue status
    Serial.printf("LoRa Queue:      %d messages\n", MessagePriorityManager::getTotalQueueDepth());
    
    // Power status
    Serial.printf("Battery:         %.2fV (%d%%)\n", 
                 PowerManager::getBatteryVoltage(), 
                 PowerManager::getBatteryPercentage());
    Serial.printf("Power State:     %s\n", 
                 PowerManager::getCurrentState() == POWER_ACTIVE ? "ACTIVE" :
                 PowerManager::getCurrentState() == POWER_IDLE ? "IDLE" :
                 PowerManager::getCurrentState() == POWER_LOW_BATTERY ? "LOW_BATTERY" : "DEEP_SLEEP");
}

void SerialDebug::showStats() {
    printSeparator();
    Serial.println("MESSAGE STATISTICS");
    printSeparator();
    
    unsigned long totalSent = StatusReporter::getTotalMessagesSent();
    unsigned long totalReceived = StatusReporter::getTotalMessagesReceived();
    unsigned long uptime = StatusReporter::getUptimeSeconds();
    
    Serial.printf("Messages Sent:     %lu\n", totalSent);
    Serial.printf("Messages Received: %lu\n", totalReceived);
    Serial.printf("Messages/Minute:   %lu\n", messagesPerMinute);
    
    if (uptime > 0) {
        Serial.printf("Avg Sent/Hour:     %.1f\n", (float)totalSent * 3600.0 / uptime);
        Serial.printf("Avg Recv/Hour:     %.1f\n", (float)totalReceived * 3600.0 / uptime);
    }
    
    // Connection statistics
    Serial.printf("Healthy BLE Conns: %d/%d\n", 
                 ConnectionManager::getHealthyConnectionCount(),
                 BLEGateway::getConnectedDeviceCount());
    
    // Queue statistics
    MessagePriorityManager::printQueueStats();
    
    // LoRa statistics
    Serial.printf("LoRa Last RSSI:    %d dBm\n", LoRaBridge::getLastRSSI());
    Serial.printf("LoRa Last SNR:     %.1f dB\n", LoRaBridge::getLastSNR());
    
    // Power statistics
    Serial.printf("Sleep Time:        %d ms\n", PowerManager::getTotalSleepTime());
    Serial.printf("Avg Power:         %.1f mA\n", PowerManager::getAveragePowerConsumption());
}

void SerialDebug::showMesh() {
    printSeparator();
    Serial.println("MESH NETWORK STATUS");
    printSeparator();
    
    Serial.println("NEIGHBOR TABLE:");
    NeighborTable::printNeighborTable();
    
    Serial.println();
    Serial.println("ROUTE TABLE:");
    RouteTable::printRouteTable();
}

void SerialDebug::pingRepeater(uint32_t repeaterId) {
    printSeparator();
    Serial.printf("PING %08X\n", repeaterId);
    printSeparator();
    
    // Check if we have a route to this repeater
    RouteEntry* route = RouteTable::findRoute(repeaterId);
    if (route) {
        Serial.printf("Route found: %d hops via %08X\n", route->hopCount, route->nextHop);
        Serial.printf("Route quality: %d%%, last used: %lu ms ago\n", 
                     route->routeQuality, millis() - route->lastUsed);
        
        // TODO: Implement actual ping functionality
        Serial.println("Ping functionality not yet implemented");
    } else {
        Serial.println("No route to repeater - initiating route discovery...");
        LoRaBridge::initiateRouteDiscovery(repeaterId);
        Serial.println("Route discovery initiated. Run 'mesh' to check for new routes.");
    }
}

void SerialDebug::traceRoute(uint32_t repeaterId) {
    printSeparator();
    Serial.printf("TRACEROUTE %08X\n", repeaterId);
    printSeparator();
    
    RouteEntry* route = RouteTable::findRoute(repeaterId);
    if (route) {
        Serial.printf("Route to %08X:\n", repeaterId);
        Serial.printf("  1. %s -> %08X (%d hops total)\n", 
                     BLEGateway::getGatewayID().c_str(), route->nextHop, route->hopCount);
        Serial.println("  ... (intermediate hops not tracked)");
        Serial.printf("  %d. %08X (destination)\n", route->hopCount + 1, repeaterId);
        
        // Show route quality information
        NeighborEntry* neighbor = NeighborTable::getNeighbor(route->nextHop);
        if (neighbor) {
            Serial.printf("Next hop quality: RSSI=%d dBm, Link=%d%%\n", 
                         neighbor->avgRSSI, neighbor->linkQuality);
        }
    } else {
        Serial.println("No route to repeater");
    }
}

void SerialDebug::resetDevice() {
    Serial.println("Resetting device in 3 seconds...");
    delay(1000);
    Serial.println("2...");
    delay(1000);
    Serial.println("1...");
    delay(1000);
    Serial.println("Resetting now!");
    ESP.restart();
}

void SerialDebug::showConfig() {
    printSeparator();
    Serial.println("DEVICE CONFIGURATION");
    printSeparator();
    
    Serial.printf("Device Name:      %s\n", ConfigManager::getDeviceName().c_str());
    Serial.printf("LoRa Region:      %s\n", 
                 ConfigManager::getLoRaRegion() == LoRaRegion::US915 ? "US915" :
                 ConfigManager::getLoRaRegion() == LoRaRegion::EU868 ? "EU868" : "AS923");
    Serial.printf("TX Power Limit:   %d dBm\n", ConfigManager::getTxPowerLimit());
    Serial.printf("Duty Cycle Limit: %d%%\n", ConfigManager::getDutyCycleLimit());
    Serial.printf("Mesh Role:        %s\n", 
                 ConfigManager::getMeshRole() == MeshRole::AUTO ? "AUTO" :
                 ConfigManager::getMeshRole() == MeshRole::ROUTER ? "ROUTER" : "LEAF");
    Serial.printf("Debug Level:      %d\n", ConfigManager::getDebugLevel());
    
    // Hardware info
    Serial.printf("Chip Model:       %s\n", ESP.getChipModel());
    Serial.printf("Chip Revision:    %d\n", ESP.getChipRevision());
    Serial.printf("Flash Size:       %s\n", formatBytes(ESP.getFlashChipSize()).c_str());
    Serial.printf("MAC Address:      %s\n", WiFi.macAddress().c_str());
}

void SerialDebug::showLoRaStats() {
    printSeparator();
    Serial.println("LORA RADIO STATISTICS");
    printSeparator();
    
    Serial.printf("Receiving:        %s\n", LoRaBridge::isReceiving() ? "Yes" : "No");
    Serial.printf("Last RSSI:        %d dBm\n", LoRaBridge::getLastRSSI());
    Serial.printf("Last SNR:         %.1f dB\n", LoRaBridge::getLastSNR());
    Serial.printf("Current TX Power: %d dBm\n", LoRaBridge::getTxPower());
    Serial.printf("Adaptive Power:   %s\n", LoRaBridge::isAdaptivePowerEnabled() ? "Enabled" : "Disabled");
    
    // Show transmission queue status
    Serial.println();
    Serial.println("TRANSMISSION QUEUE:");
    // Queue details would be shown here if accessible
    Serial.println("(Queue details not accessible from debug interface)");
    
    // Show neighbor-specific LoRa info
    Serial.println();
    Serial.println("NEIGHBOR LoRa INFO:");
    auto& neighbors = NeighborTable::getAllNeighbors();
    if (neighbors.empty()) {
        Serial.println("No neighbors discovered");
    } else {
        Serial.printf("%-10s %-8s %-8s %-8s\n", "Repeater", "RSSI", "Quality", "Opt SF");
        for (const auto& pair : neighbors) {
            const NeighborEntry& neighbor = pair.second;
            Serial.printf("%08X   %-8d %-8d %-8d\n", 
                         neighbor.repeaterId, neighbor.avgRSSI, 
                         neighbor.linkQuality, neighbor.optimalSF);
        }
    }
}

void SerialDebug::showBLEStats() {
    printSeparator();
    Serial.println("BLE CONNECTION STATISTICS");
    printSeparator();
    
    Serial.printf("Connected iOS Devices: %d\n", BLEGateway::getConnectedDeviceCount());
    Serial.printf("Healthy Connections: %d\n", ConnectionManager::getHealthyConnectionCount());
    Serial.printf("Best Connection Score: %d%%\n", ConnectionManager::getBestConnectionScore());
    
    // Show detailed connection information
    Serial.println();
    ConnectionManager::printConnectionStats();
}

void SerialDebug::setLogLevel(uint8_t level) {
    ConfigManager::setDebugLevel(level);
    Serial.printf("Debug level set to %d\n", level);
    Serial.println("Levels: 0=Off, 1=Error, 2=Warning, 3=Debug");
}

// Utility functions
void SerialDebug::printTable(const String& title, const String headers[], const String data[][10], int rows, int cols) {
    printSeparator();
    Serial.println(title);
    printSeparator();
    
    // Print headers
    for (int i = 0; i < cols; i++) {
        Serial.printf("%-12s ", headers[i].c_str());
    }
    Serial.println();
    
    // Print separator line
    for (int i = 0; i < cols; i++) {
        Serial.print("------------ ");
    }
    Serial.println();
    
    // Print data rows
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            Serial.printf("%-12s ", data[row][col].c_str());
        }
        Serial.println();
    }
}

void SerialDebug::printSeparator(int width) {
    for (int i = 0; i < width; i++) {
        Serial.print("-");
    }
    Serial.println();
}

String SerialDebug::formatUptime(unsigned long seconds) {
    unsigned long days = seconds / 86400;
    seconds %= 86400;
    unsigned long hours = seconds / 3600;
    seconds %= 3600;
    unsigned long minutes = seconds / 60;
    seconds %= 60;
    
    String result = "";
    if (days > 0) result += String(days) + "d ";
    if (hours > 0 || days > 0) result += String(hours) + "h ";
    if (minutes > 0 || hours > 0 || days > 0) result += String(minutes) + "m ";
    result += String(seconds) + "s";
    
    return result;
}

String SerialDebug::formatBytes(size_t bytes) {
    if (bytes < 1024) return String(bytes) + "B";
    if (bytes < 1024 * 1024) return String(bytes / 1024.0, 1) + "KB";
    if (bytes < 1024 * 1024 * 1024) return String(bytes / (1024.0 * 1024.0), 1) + "MB";
    return String(bytes / (1024.0 * 1024.0 * 1024.0), 1) + "GB";
}

uint32_t SerialDebug::parseHexId(const String& hexStr) {
    if (!isValidHexId(hexStr)) return 0;
    
    // Convert hex string to uint32_t
    return (uint32_t)strtoul(hexStr.c_str(), NULL, 16);
}

bool SerialDebug::isValidHexId(const String& hexStr) {
    if (hexStr.length() != 8) return false;
    
    for (int i = 0; i < 8; i++) {
        char c = hexStr.charAt(i);
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    
    return true;
}