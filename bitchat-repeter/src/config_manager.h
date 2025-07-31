#pragma once
#include <Preferences.h>
#include <Arduino.h>

enum class LoRaRegion {
    US915,
    EU868,
    AS923
};

enum class MeshRole {
    AUTO,
    ROUTER,
    LEAF
};

struct DeviceConfig {
    String deviceName;
    LoRaRegion loraRegion;
    uint8_t txPowerLimit;
    uint8_t dutyCycleLimit;
    MeshRole meshRole;
    uint8_t debugLevel;
    uint8_t configVersion;
};

class ConfigManager {
public:
    static void init();
    static void loadConfig();
    static void saveConfig();
    static void resetToDefaults();
    static bool isButtonPressedDuringBoot();
    
    // Getters
    static String getDeviceName();
    static LoRaRegion getLoRaRegion();
    static uint8_t getTxPowerLimit();
    static uint8_t getDutyCycleLimit();
    static MeshRole getMeshRole();
    static uint8_t getDebugLevel();
    
    // Setters
    static void setDeviceName(const String& name);
    static void setLoRaRegion(LoRaRegion region);
    static void setTxPowerLimit(uint8_t power);
    static void setDutyCycleLimit(uint8_t limit);
    static void setMeshRole(MeshRole role);
    static void setDebugLevel(uint8_t level);
    
private:
    static Preferences preferences;
    static DeviceConfig config;
    static const uint8_t CURRENT_CONFIG_VERSION = 1;
    static const char* NAMESPACE;
    
    static void createDefaultConfig();
    static String generateDeviceName();
};