#include "utils/config_manager.h"
#include "hardware/hardware_config.h"
#include <WiFi.h>

// Static member definitions
Preferences ConfigManager::preferences;
DeviceConfig ConfigManager::config;
const char* ConfigManager::NAMESPACE = "bitchat";

void ConfigManager::init() {
    Serial.println("Config Manager: Initializing...");
    
    // Check if button is pressed during boot for configuration reset
    if (isButtonPressedDuringBoot()) {
        Serial.println("Config Manager: Button pressed during boot - resetting to defaults");
        resetToDefaults();
    } else {
        loadConfig();
    }
    
    Serial.printf("Config Manager: Device name: %s\n", config.deviceName.c_str());
    Serial.printf("Config Manager: LoRa region: %d\n", (int)config.loraRegion);
    Serial.printf("Config Manager: TX power limit: %d dBm\n", config.txPowerLimit);
    Serial.printf("Config Manager: Duty cycle limit: %d%%\n", config.dutyCycleLimit);
    Serial.printf("Config Manager: Mesh role: %d\n", (int)config.meshRole);
    Serial.printf("Config Manager: Debug level: %d\n", config.debugLevel);
    Serial.println("Config Manager: Initialized successfully");
}

void ConfigManager::loadConfig() {
    if (!preferences.begin(NAMESPACE, false)) {
        Serial.println("Config Manager: Failed to open preferences namespace");
        createDefaultConfig();
        return;
    }
    
    // Check if configuration exists and version is compatible
    uint8_t version = preferences.getUChar("version", 0);
    if (version == 0 || version != CURRENT_CONFIG_VERSION) {
        Serial.printf("Config Manager: Config version mismatch (found %d, expected %d) - creating defaults\n", 
                     version, CURRENT_CONFIG_VERSION);
        preferences.end();
        createDefaultConfig();
        return;
    }
    
    // Load configuration
    config.deviceName = preferences.getString("deviceName", generateDeviceName());
    config.loraRegion = (LoRaRegion)preferences.getUChar("loraRegion", (uint8_t)LoRaRegion::US915);
    config.txPowerLimit = preferences.getUChar("txPowerLimit", 20);
    config.dutyCycleLimit = preferences.getUChar("dutyCycleLimit", 1);
    config.meshRole = (MeshRole)preferences.getUChar("meshRole", (uint8_t)MeshRole::AUTO);
    config.debugLevel = preferences.getUChar("debugLevel", 1);
    config.configVersion = version;
    
    preferences.end();
    Serial.println("Config Manager: Configuration loaded from NVS");
}

void ConfigManager::saveConfig() {
    if (!preferences.begin(NAMESPACE, false)) {
        Serial.println("Config Manager: Failed to open preferences namespace for saving");
        return;
    }
    
    preferences.putString("deviceName", config.deviceName);
    preferences.putUChar("loraRegion", (uint8_t)config.loraRegion);
    preferences.putUChar("txPowerLimit", config.txPowerLimit);
    preferences.putUChar("dutyCycleLimit", config.dutyCycleLimit);
    preferences.putUChar("meshRole", (uint8_t)config.meshRole);
    preferences.putUChar("debugLevel", config.debugLevel);
    preferences.putUChar("version", CURRENT_CONFIG_VERSION);
    
    preferences.end();
    Serial.println("Config Manager: Configuration saved to NVS");
}

void ConfigManager::resetToDefaults() {
    if (!preferences.begin(NAMESPACE, false)) {
        Serial.println("Config Manager: Failed to open preferences namespace for reset");
        createDefaultConfig();
        return;
    }
    
    preferences.clear();
    preferences.end();
    
    createDefaultConfig();
    saveConfig();
    Serial.println("Config Manager: Configuration reset to defaults");
}

bool ConfigManager::isButtonPressedDuringBoot() {
    // Check if user button is pressed during boot
    // On Heltec WiFi LoRa 32 V3, user button is on pin 0
    pinMode(0, INPUT_PULLUP);
    delay(50); // Allow pin to stabilize
    return digitalRead(0) == LOW;
}

void ConfigManager::createDefaultConfig() {
    config.deviceName = generateDeviceName();
    config.loraRegion = LoRaRegion::US915;
    config.txPowerLimit = 20;
    config.dutyCycleLimit = 1;
    config.meshRole = MeshRole::AUTO;
    config.debugLevel = 1;
    config.configVersion = CURRENT_CONFIG_VERSION;
}

String ConfigManager::generateDeviceName() {
    // Generate device name from MAC address
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char name[16];
    snprintf(name, sizeof(name), "Repeater-%02X", mac[5]);
    return String(name);
}

// Getters
String ConfigManager::getDeviceName() {
    return config.deviceName;
}

LoRaRegion ConfigManager::getLoRaRegion() {
    return config.loraRegion;
}

uint8_t ConfigManager::getTxPowerLimit() {
    return config.txPowerLimit;
}

uint8_t ConfigManager::getDutyCycleLimit() {
    return config.dutyCycleLimit;
}

MeshRole ConfigManager::getMeshRole() {
    return config.meshRole;
}

uint8_t ConfigManager::getDebugLevel() {
    return config.debugLevel;
}

// Setters
void ConfigManager::setDeviceName(const String& name) {
    config.deviceName = name;
    saveConfig();
}

void ConfigManager::setLoRaRegion(LoRaRegion region) {
    config.loraRegion = region;
    saveConfig();
}

void ConfigManager::setTxPowerLimit(uint8_t power) {
    if (power > 20) power = 20; // Clamp to legal limit
    config.txPowerLimit = power;
    saveConfig();
}

void ConfigManager::setDutyCycleLimit(uint8_t limit) {
    if (limit > 100) limit = 100; // Clamp to 100%
    config.dutyCycleLimit = limit;
    saveConfig();
}

void ConfigManager::setMeshRole(MeshRole role) {
    config.meshRole = role;
    saveConfig();
}

void ConfigManager::setDebugLevel(uint8_t level) {
    if (level > 3) level = 3; // Clamp to valid range
    config.debugLevel = level;
    saveConfig();
}