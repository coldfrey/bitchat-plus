#include <Arduino.h>
#include "hardware_config.h"
#include "ble_mesh.h"
#include "lora_bridge.h"
#include "message_router.h"
#include "config_manager.h"

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("BitChat Repeater starting...");
    
    // Initialize LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // Turn on LED during startup
    
    // Initialize button
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    // Enable Vext power for peripherals
    pinMode(VEXT_ENABLE, OUTPUT);
    digitalWrite(VEXT_ENABLE, LOW);  // Enable Vext
    
    // Initialize modules
    // ConfigManager::init();
    MessageRouter::init();  // Initialize message router first
    BLEMesh::init();
    LoRaBridge::init();     // Initialize LoRa radio
    
    digitalWrite(LED_PIN, LOW);  // Turn off LED after startup
    Serial.println("BitChat Repeater ready");
}

void loop() {
    // Main loop processing
    BLEMesh::process();
    LoRaBridge::process();     // Process LoRa radio
    MessageRouter::process();  // Process deduplication cleanup
    
    delay(10);  // Small delay to prevent watchdog reset
}