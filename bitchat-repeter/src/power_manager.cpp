#include "power_manager.h"
#include "hardware_config.h"
#include "ble_gateway.h"
#include "lora_bridge.h"
#include <esp_sleep.h>
#include <esp_adc_cal.h>
#include <Arduino.h>

// Static member definitions
PowerState PowerManager::currentState = POWER_ACTIVE;
unsigned long PowerManager::lastActivity = 0;
unsigned long PowerManager::lastBatteryCheck = 0;
unsigned long PowerManager::lastStateCheck = 0;
unsigned long PowerManager::totalSleepTime = 0;

float PowerManager::batteryVoltage = 0.0;
uint8_t PowerManager::batteryPercentage = 100;
BatteryLevel PowerManager::batteryLevel = BATTERY_FULL;
unsigned long PowerManager::batteryCheckInterval = 10000;

int8_t PowerManager::currentTxPower = LORA_TX_POWER_MAX;
unsigned long PowerManager::buttonPressStart = 0;
bool PowerManager::buttonPressed = false;

void PowerManager::init() {
    Serial.println("PowerManager: Initializing power management...");
    
    // Initialize ADC for battery monitoring
    analogReadResolution(12); // 12-bit resolution (0-4095)
    analogSetAttenuation(ADC_11db); // For 3.3V range
    
    // Configure button pin for wake-up
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    // Configure sleep wake-up sources
    configureSleepWakeup();
    
    // Initial state
    currentState = POWER_ACTIVE;
    lastActivity = millis();
    lastBatteryCheck = millis();
    lastStateCheck = millis();
    
    // Initial battery check
    updateBatteryStatus();
    
    Serial.printf("PowerManager: Initialized - Battery: %.2fV (%d%%), State: %s\n",
                 batteryVoltage, batteryPercentage, 
                 currentState == POWER_ACTIVE ? "ACTIVE" : "OTHER");
}

void PowerManager::process() {
    unsigned long now = millis();
    
    // Check battery status periodically
    if (now - lastBatteryCheck > BATTERY_CHECK_INTERVAL) {
        updateBatteryStatus();
        lastBatteryCheck = now;
    }
    
    // Update power state periodically
    if (now - lastStateCheck > STATE_CHECK_INTERVAL) {
        updatePowerState();
        lastStateCheck = now;
    }
    
    // Check for deep sleep button press
    checkButtonForDeepSleep();
    
    // Handle idle light sleep
    if (currentState == POWER_IDLE && isIdle()) {
        // Enter light sleep for short periods when idle
        if (getIdleTime() > IDLE_THRESHOLD_MS) {
            Serial.println("PowerManager: Entering light sleep (idle)");
            enterLightSleep(100); // 100ms light sleep
        }
    }
}

PowerState PowerManager::getCurrentState() {
    return currentState;
}

void PowerManager::setState(PowerState state) {
    if (currentState != state) {
        Serial.printf("PowerManager: State change: %s -> %s\n",
                     currentState == POWER_ACTIVE ? "ACTIVE" : 
                     currentState == POWER_IDLE ? "IDLE" :
                     currentState == POWER_LOW_BATTERY ? "LOW_BATTERY" : "DEEP_SLEEP",
                     state == POWER_ACTIVE ? "ACTIVE" : 
                     state == POWER_IDLE ? "IDLE" :
                     state == POWER_LOW_BATTERY ? "LOW_BATTERY" : "DEEP_SLEEP");
        
        currentState = state;
        updateBLEAdvertising();
    }
}

bool PowerManager::shouldReduceActivity() {
    return currentState == POWER_LOW_BATTERY || currentState == POWER_IDLE;
}

float PowerManager::getBatteryVoltage() {
    return batteryVoltage;
}

uint8_t PowerManager::getBatteryPercentage() {
    return batteryPercentage;
}

BatteryLevel PowerManager::getBatteryLevel() {
    return batteryLevel;
}

bool PowerManager::isBatteryLow() {
    return batteryPercentage <= LOW_BATTERY_THRESHOLD;
}

bool PowerManager::isBatteryCritical() {
    return batteryPercentage <= CRITICAL_BATTERY_THRESHOLD;
}

void PowerManager::enterLightSleep(uint32_t durationMs) {
    if (durationMs < 10) return; // Too short to be worth it
    
    Serial.printf("PowerManager: Entering light sleep for %dms\n", durationMs);
    
    // Configure light sleep
    esp_sleep_enable_timer_wakeup(durationMs * 1000); // Convert to microseconds
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Button wake-up
    
    unsigned long sleepStart = millis();
    esp_light_sleep_start();
    unsigned long sleepEnd = millis();
    
    totalSleepTime += (sleepEnd - sleepStart);
    recordActivity(); // Record wake-up as activity
    
    Serial.printf("PowerManager: Woke from light sleep after %dms\n", sleepEnd - sleepStart);
}

void PowerManager::enterDeepSleep() {
    Serial.println("PowerManager: Entering deep sleep...");
    
    // Configure deep sleep wake-up
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Button wake-up
    
    // Turn off peripherals
    digitalWrite(LED_PIN, LOW);
    digitalWrite(VEXT_ENABLE, HIGH); // Disable Vext
    
    // Go to deep sleep
    esp_deep_sleep_start();
}

void PowerManager::wakeFromSleep() {
    recordActivity();
    Serial.println("PowerManager: Woke from sleep");
}

void PowerManager::recordActivity() {
    lastActivity = millis();
    if (currentState == POWER_IDLE) {
        setState(POWER_ACTIVE);
    }
}

bool PowerManager::isIdle() {
    return (millis() - lastActivity) > IDLE_THRESHOLD_MS;
}

unsigned long PowerManager::getIdleTime() {
    return millis() - lastActivity;
}

uint32_t PowerManager::getBLEAdvertisingInterval() {
    switch (currentState) {
        case POWER_ACTIVE:
            return BLE_ADV_ACTIVE_MS;
        case POWER_IDLE:
            return BLE_ADV_IDLE_MS;
        case POWER_LOW_BATTERY:
            return BLE_ADV_LOW_BATTERY_MS;
        default:
            return BLE_ADV_IDLE_MS;
    }
}

void PowerManager::updateBLEAdvertising() {
    uint32_t interval = getBLEAdvertisingInterval();
    Serial.printf("PowerManager: Updating BLE advertising interval to %dms\n", interval);
    BLEGateway::setAdvertisingInterval(interval);
}

int8_t PowerManager::getOptimalTxPower(int16_t neighborRSSI) {
    // Adaptive TX power based on neighbor RSSI
    // Better RSSI = lower power needed
    
    if (currentState == POWER_LOW_BATTERY) {
        return LORA_TX_POWER_LOW_BATTERY;
    }
    
    if (neighborRSSI > -70) {
        // Strong signal, can use lower power
        return LORA_TX_POWER_REDUCED;
    } else if (neighborRSSI > -90) {
        // Medium signal, use medium power
        return (LORA_TX_POWER_MAX + LORA_TX_POWER_REDUCED) / 2;
    } else {
        // Weak signal, use full power
        return LORA_TX_POWER_MAX;
    }
}

void PowerManager::reduceTxPower() {
    currentTxPower = LORA_TX_POWER_REDUCED;
    LoRaBridge::setTxPower(currentTxPower);
    Serial.printf("PowerManager: Reduced TX power to %d dBm\n", currentTxPower);
}

void PowerManager::restoreTxPower() {
    currentTxPower = LORA_TX_POWER_MAX;
    LoRaBridge::setTxPower(currentTxPower);
    Serial.printf("PowerManager: Restored TX power to %d dBm\n", currentTxPower);
}

void PowerManager::checkButtonForDeepSleep() {
    bool buttonCurrentlyPressed = (digitalRead(BUTTON_PIN) == LOW);
    
    if (buttonCurrentlyPressed && !buttonPressed) {
        // Button just pressed
        buttonPressed = true;
        buttonPressStart = millis();
    } else if (!buttonCurrentlyPressed && buttonPressed) {
        // Button just released
        buttonPressed = false;
        unsigned long holdTime = millis() - buttonPressStart;
        
        if (holdTime >= DEEP_SLEEP_BUTTON_HOLD_MS) {
            Serial.printf("PowerManager: Button held for %dms - entering deep sleep\n", holdTime);
            enterDeepSleep();
        } else {
            Serial.printf("PowerManager: Button pressed for %dms - recording activity\n", holdTime);
            recordActivity();
        }
    }
}

bool PowerManager::isButtonHeld(uint32_t durationMs) {
    return buttonPressed && (millis() - buttonPressStart) >= durationMs;
}

void PowerManager::printPowerStats() {
    Serial.println("=== Power Management Statistics ===");
    Serial.printf("Current State: %s\n", 
                 currentState == POWER_ACTIVE ? "ACTIVE" : 
                 currentState == POWER_IDLE ? "IDLE" :
                 currentState == POWER_LOW_BATTERY ? "LOW_BATTERY" : "DEEP_SLEEP");
    Serial.printf("Battery: %.2fV (%d%%) - %s\n", 
                 batteryVoltage, batteryPercentage,
                 batteryLevel == BATTERY_FULL ? "FULL" :
                 batteryLevel == BATTERY_HIGH ? "HIGH" :
                 batteryLevel == BATTERY_MEDIUM ? "MEDIUM" :
                 batteryLevel == BATTERY_LOW ? "LOW" : "CRITICAL");
    Serial.printf("Idle Time: %ds\n", getIdleTime() / 1000);
    Serial.printf("Total Sleep Time: %ds\n", totalSleepTime / 1000);
    Serial.printf("Current TX Power: %d dBm\n", currentTxPower);
    Serial.printf("BLE Advertising: %dms interval\n", getBLEAdvertisingInterval());
    Serial.println("=====================================");
}

unsigned long PowerManager::getTotalSleepTime() {
    return totalSleepTime;
}

float PowerManager::getAveragePowerConsumption() {
    // Rough estimate based on state and activity
    unsigned long uptime = millis();
    float activeFraction = 1.0 - (float)totalSleepTime / uptime;
    
    // Rough power consumption estimates (mA)
    float activePower = 150.0;  // Active with BLE + LoRa
    float sleepPower = 10.0;    // Light sleep
    
    return activePower * activeFraction + sleepPower * (1.0 - activeFraction);
}

void PowerManager::updateBatteryStatus() {
    // Read ADC value
    uint16_t adcReading = analogRead(BATTERY_ADC);
    
    // Convert to voltage
    batteryVoltage = adcToVoltage(adcReading);
    
    // Convert to percentage
    batteryPercentage = voltageToPercentage(batteryVoltage);
    
    // Determine battery level
    if (batteryPercentage > 80) {
        batteryLevel = BATTERY_FULL;
    } else if (batteryPercentage > 50) {
        batteryLevel = BATTERY_HIGH;
    } else if (batteryPercentage > 20) {
        batteryLevel = BATTERY_MEDIUM;
    } else if (batteryPercentage > 10) {
        batteryLevel = BATTERY_LOW;
    } else {
        batteryLevel = BATTERY_CRITICAL;
    }
    
    Serial.printf("PowerManager: Battery status - %.2fV (%d%%) - %s\n", 
                 batteryVoltage, batteryPercentage,
                 batteryLevel == BATTERY_FULL ? "FULL" :
                 batteryLevel == BATTERY_HIGH ? "HIGH" :
                 batteryLevel == BATTERY_MEDIUM ? "MEDIUM" :
                 batteryLevel == BATTERY_LOW ? "LOW" : "CRITICAL");
}

void PowerManager::updatePowerState() {
    PowerState newState = currentState;
    PowerState oldState = currentState;
    
    if (isBatteryCritical()) {
        newState = POWER_LOW_BATTERY;
    } else if (isBatteryLow()) {
        newState = POWER_LOW_BATTERY;
    } else if (isIdle()) {
        newState = POWER_IDLE;
    } else {
        newState = POWER_ACTIVE;
    }
    
    setState(newState);
    
    // Adjust TX power based on power state change
    if (oldState != newState) {
        if (newState == POWER_LOW_BATTERY && LoRaBridge::getTxPower() > LORA_TX_POWER_LOW_BATTERY) {
            LoRaBridge::setTxPower(LORA_TX_POWER_LOW_BATTERY);
            Serial.printf("PowerManager: Reduced TX power to %d dBm due to low battery\n", LORA_TX_POWER_LOW_BATTERY);
        } else if (newState == POWER_ACTIVE && oldState == POWER_LOW_BATTERY) {
            LoRaBridge::setTxPower(LORA_TX_POWER_MAX);
            Serial.printf("PowerManager: Restored TX power to %d dBm\n", LORA_TX_POWER_MAX);
        }
    }
}

float PowerManager::adcToVoltage(uint16_t adcReading) {
    // Heltec V3 has voltage divider: Vbat -> 320k -> ADC -> 100k -> GND
    // Divider ratio = 100k / (320k + 100k) = 0.238
    // So Vbat = Vadc / 0.238
    
    // ESP32-S3 ADC reference is ~3.3V with 12-bit resolution (4095)
    float adcVoltage = (float)adcReading * 3.3 / 4095.0;
    float batteryVoltage = adcVoltage / 0.238;
    
    return batteryVoltage;
}

uint8_t PowerManager::voltageToPercentage(float voltage) {
    // Li-Po battery voltage curve approximation
    // 4.2V = 100%, 3.7V = 50%, 3.3V = 10%, 3.0V = 0%
    
    if (voltage >= 4.2) return 100;
    if (voltage >= 4.0) return 80 + (voltage - 4.0) * 100; // 80-100%
    if (voltage >= 3.8) return 60 + (voltage - 3.8) * 100; // 60-80%
    if (voltage >= 3.6) return 40 + (voltage - 3.6) * 100; // 40-60%
    if (voltage >= 3.4) return 20 + (voltage - 3.4) * 100; // 20-40%
    if (voltage >= 3.2) return 10 + (voltage - 3.2) * 50;  // 10-20%
    if (voltage >= 3.0) return (voltage - 3.0) * 50;       // 0-10%
    
    return 0;
}

void PowerManager::configureSleepWakeup() {
    // Configure button as wake-up source
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // Wake on button press (LOW)
    
    // Enable wake-up from light sleep on timer and button
    rtc_gpio_pullup_en(GPIO_NUM_0);
    rtc_gpio_pulldown_dis(GPIO_NUM_0);
}