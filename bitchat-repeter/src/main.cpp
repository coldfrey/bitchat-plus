#include <Arduino.h>
#include "hardware_config.h"
#include "ble_mesh.h"
#include "lora_bridge.h"
#include "message_router.h"
#include "config_manager.h"
#include "bitchat_protocol.h"

// Test function to verify LoRa packet format works correctly
void testLoRaPacketFormat() {
    Serial.println("=== Testing LoRa Packet Format ===");
    
    // Create a test LoRa packet
    LoRaPacket testPacket;
    testPacket.type = LORA_PKT_DATA;
    testPacket.srcRepeater = 0x12345678;
    testPacket.destRepeater = 0xABCDEF00;
    testPacket.nextHop = 0x11223344;
    testPacket.hopCount = 2;
    testPacket.maxHops = 5;
    testPacket.seqNum = 0x1234;
    
    // Create test payload (simulate a BitChat packet)
    const char* testData = "Hello, LoRa mesh!";
    testPacket.payloadLen = strlen(testData);
    memcpy(testPacket.payload, testData, testPacket.payloadLen);
    
    // Serialize the packet
    uint8_t buffer[256];
    size_t serializedSize = serializeLoRaPacket(testPacket, buffer, sizeof(buffer));
    
    if (serializedSize == 0) {
        Serial.println("ERROR: Failed to serialize LoRa packet");
        return;
    }
    
    Serial.printf("Serialized packet: %d bytes\n", serializedSize);
    
    // Parse the serialized packet back
    LoRaPacket parsedPacket;
    LoRaParseResult result = parseLoRaPacket(buffer, serializedSize, parsedPacket);
    
    if (result != LORA_PARSE_SUCCESS) {
        Serial.printf("ERROR: Failed to parse LoRa packet (error code: %d)\n", result);
        return;
    }
    
    // Verify the parsed packet matches the original
    bool success = true;
    if (parsedPacket.type != testPacket.type) {
        Serial.printf("ERROR: Type mismatch (expected 0x%02X, got 0x%02X)\n", testPacket.type, parsedPacket.type);
        success = false;
    }
    if (parsedPacket.srcRepeater != testPacket.srcRepeater) {
        Serial.printf("ERROR: Source mismatch (expected %08X, got %08X)\n", testPacket.srcRepeater, parsedPacket.srcRepeater);
        success = false;
    }
    if (parsedPacket.destRepeater != testPacket.destRepeater) {
        Serial.printf("ERROR: Destination mismatch (expected %08X, got %08X)\n", testPacket.destRepeater, parsedPacket.destRepeater);
        success = false;
    }
    if (parsedPacket.payloadLen != testPacket.payloadLen) {
        Serial.printf("ERROR: Payload length mismatch (expected %d, got %d)\n", testPacket.payloadLen, parsedPacket.payloadLen);
        success = false;
    }
    if (memcmp(parsedPacket.payload, testPacket.payload, testPacket.payloadLen) != 0) {
        Serial.println("ERROR: Payload data mismatch");
        success = false;
    }
    
    if (success) {
        Serial.println("SUCCESS: LoRa packet format test passed!");
        Serial.printf("  Packet size: %d bytes (header=%d, payload=%d)\n", 
                     serializedSize, LORA_HEADER_SIZE, testPacket.payloadLen);
        Serial.printf("  All fields correctly serialized and parsed\n");
    } else {
        Serial.println("FAILED: LoRa packet format test failed!");
    }
    
    Serial.println("=== End LoRa Packet Format Test ===");
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("BitChat Repeater starting...");
    
    // Test LoRa packet format
    testLoRaPacketFormat();
    
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