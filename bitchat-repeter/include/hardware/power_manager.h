#pragma once

#include <Arduino.h>
#include <driver/rtc_io.h>

// Power management states
enum PowerState {
    POWER_ACTIVE,      // Normal operation
    POWER_IDLE,        // Reduced activity
    POWER_LOW_BATTERY, // Battery saving mode
    POWER_DEEP_SLEEP   // Deep sleep mode
};

// Battery levels
enum BatteryLevel {
    BATTERY_CRITICAL = 0,  // < 10%
    BATTERY_LOW = 1,       // 10-20%
    BATTERY_MEDIUM = 2,    // 20-50%
    BATTERY_HIGH = 3,      // 50-80%
    BATTERY_FULL = 4       // > 80%
};

class PowerManager {
public:
    static void init();
    static void process();
    
    // Power state management
    static PowerState getCurrentState();
    static void setState(PowerState state);
    static bool shouldReduceActivity();
    
    // Battery monitoring
    static float getBatteryVoltage();
    static uint8_t getBatteryPercentage();
    static BatteryLevel getBatteryLevel();
    static bool isBatteryLow();
    static bool isBatteryCritical();
    
    // Sleep management
    static void enterLightSleep(uint32_t durationMs);
    static void enterDeepSleep();
    static void wakeFromSleep();
    
    // Activity monitoring
    static void recordActivity();
    static bool isIdle();
    static unsigned long getIdleTime();
    
    // BLE advertising control
    static uint32_t getBLEAdvertisingInterval();
    static void updateBLEAdvertising();
    
    // LoRa power control
    static int8_t getOptimalTxPower(int16_t neighborRSSI);
    static void reduceTxPower();
    static void restoreTxPower();
    
    // Button handling for deep sleep
    static void checkButtonForDeepSleep();
    static bool isButtonHeld(uint32_t durationMs);
    
    // Statistics
    static void printPowerStats();
    static unsigned long getTotalSleepTime();
    static float getAveragePowerConsumption();

private:
    static PowerState currentState;
    static unsigned long lastActivity;
    static unsigned long lastBatteryCheck;
    static unsigned long lastStateCheck;
    static unsigned long totalSleepTime;
    
    // Battery monitoring
    static float batteryVoltage;
    static uint8_t batteryPercentage;
    static BatteryLevel batteryLevel;
    static unsigned long batteryCheckInterval;
    
    // Activity tracking
    static const unsigned long IDLE_THRESHOLD_MS = 30000;      // 30 seconds
    static const unsigned long LOW_BATTERY_THRESHOLD = 20;     // 20%
    static const unsigned long CRITICAL_BATTERY_THRESHOLD = 10; // 10%
    static const unsigned long BATTERY_CHECK_INTERVAL = 10000; // 10 seconds
    static const unsigned long STATE_CHECK_INTERVAL = 5000;    // 5 seconds
    
    // BLE advertising intervals
    static const uint32_t BLE_ADV_ACTIVE_MS = 100;    // 100ms when active
    static const uint32_t BLE_ADV_IDLE_MS = 1000;     // 1000ms when idle
    static const uint32_t BLE_ADV_LOW_BATTERY_MS = 2000; // 2000ms when low battery
    
    // LoRa power settings
    static const int8_t LORA_TX_POWER_MAX = 20;       // 20 dBm max
    static const int8_t LORA_TX_POWER_REDUCED = 14;   // 14 dBm reduced
    static const int8_t LORA_TX_POWER_LOW_BATTERY = 10; // 10 dBm low battery
    static int8_t currentTxPower;
    
    // Button handling
    static const uint32_t DEEP_SLEEP_BUTTON_HOLD_MS = 3000; // 3 seconds
    static unsigned long buttonPressStart;
    static bool buttonPressed;
    
    // Helper functions
    static void updateBatteryStatus();
    static void updatePowerState();
    static float adcToVoltage(uint16_t adcReading);
    static uint8_t voltageToPercentage(float voltage);
    static void configureSleepWakeup();
};