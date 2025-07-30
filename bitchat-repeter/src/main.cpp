// Heltec WiFi LoRa 32 V3 with OLED Display and Button Communication
// This file demonstrates LoRa communication between two devices with button press messages
// For: ESP32-S3 + SX1262 + SSD1306 OLED (128x64)
// Press the PRG button to send a message to the other device
// Also acts as a BLE peripheral to receive messages via BLE and forward them over LoRa

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "heltec.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// --- BLE UUIDs ---
#define SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // Nordic UART Service
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// --- Button pin ---
#define BUTTON_PIN 0  // PRG button on Heltec V3

// --- Heltec V3 radio pins (SX1262) ---
#define LORA_CS   8
#define LORA_IRQ  14  // DIO1
#define LORA_RST  12
#define LORA_BUSY 13

// --- Radio params (set for your region) ---
static const float   FREQ_MHZ   = 915.0;   // US915 - change to 868.1 for EU
static const float   BW_KHZ     = 125.0;
static const uint8_t SF         = 9;
static const uint8_t CR_DEN     = 5;
static const int8_t  PWR_DBM    = 14;

// --- Globals ---
SX1262 radio = new Module(LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);

// BLE globals
BLEServer* pServer = NULL;
BLECharacteristic* pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;
String bleMessageToSend = "";
bool hasBleMessage = false;

uint64_t devId = 0;
uint32_t msgCount = 0;
uint32_t rxCount = 0;
uint32_t txCount = 0;
float lastRSSI = 0;
float lastSNR = 0;
String lastRxMsg = "";
String lastTxMsg = "";  // Add this to track last sent message
String lastMsgSource = "";  // Track if message was from BLE or button
String lastBleRxMsg = "";  // Track last BLE received message
bool buttonPressed = false;
volatile bool receivedFlag = false;
volatile bool enableInterrupt = true;
unsigned long lastButtonPress = 0;
unsigned long lastDebugPrint = 0;
const unsigned long DEBOUNCE_DELAY = 200;

// BLE Server Callbacks
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("BLE Client Connected");
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      Serial.println("BLE Client Disconnected");
    }
};

// BLE Characteristic Callbacks
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();

      if (rxValue.length() > 0) {
        bleMessageToSend = "";
        for (int i = 0; i < rxValue.length(); i++) {
          bleMessageToSend += rxValue[i];
        }
        // Remove any trailing newlines or carriage returns
        bleMessageToSend.trim();
        hasBleMessage = true;
        lastBleRxMsg = bleMessageToSend;  // Store the received BLE message
        Serial.println("BLE RX: " + bleMessageToSend);
      }
    }
};

// ISR for radio receive
void setFlag(void) {
  if (!enableInterrupt) return;
  receivedFlag = true;
}

void updateDisplay() {
  Heltec.display->clear();
  
  // Title with device ID and BLE status - Line 0
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
  Heltec.display->setFont(ArialMT_Plain_10);
  char idStr[8];
  sprintf(idStr, "%06X", (uint32_t)(devId & 0xFFFFFF));
  String title = "ID:" + String(idStr);
  if (deviceConnected) {
    title += " BLE:ON";
  } else {
    title += " BLE:OFF";
  }
  Heltec.display->drawString(0, 0, title);
  
  // Draw a line separator - Line 12
  Heltec.display->drawLine(0, 12, 128, 12);
  
  // Stats line - Line 14
  Heltec.display->setFont(ArialMT_Plain_10);
  String stats = "TX:" + String(txCount) + " RX:" + String(rxCount);
  if (rxCount > 0) {
    stats += " " + String(lastRSSI, 0) + "dBm";
  }
  Heltec.display->drawString(0, 14, stats);
  
  // Compact layout to fit everything in 64 pixels
  int yPos = 26;
  
  // Last sent message (if any) - more compact
  if (txCount > 0) {
    String sentLabel = "TX(" + lastMsgSource + "):";
    String sentMsg = lastTxMsg;
    if (sentMsg.length() > 15) {
      sentMsg = sentMsg.substring(0, 15) + "...";
    }
    Heltec.display->drawString(0, yPos, sentLabel + sentMsg);
    yPos += 10;
  }
  
  // Last LoRa received message (if any)
  if (rxCount > 0) {
    String loraMsg = lastRxMsg;
    if (loraMsg.length() > 15) {
      loraMsg = loraMsg.substring(0, 15) + "...";
    }
    Heltec.display->drawString(0, yPos, "LoRa:" + loraMsg);
    yPos += 10;
  }
  
  // Last BLE received message (if any)
  if (lastBleRxMsg.length() > 0) {
    String bleMsg = lastBleRxMsg;
    if (bleMsg.length() > 15) {
      bleMsg = bleMsg.substring(0, 15) + "...";
    }
    Heltec.display->drawString(0, yPos, "BLE:" + bleMsg);
    yPos += 10;
  }
  
  // Show instructions only if nothing has happened yet
  if (txCount == 0 && rxCount == 0 && lastBleRxMsg.length() == 0) {
    Heltec.display->setFont(ArialMT_Plain_10);
    Heltec.display->drawString(0, 35, "Press button or");
    Heltec.display->drawString(0, 45, "send via BLE!");
  }
  
  Heltec.display->display();
}

void sendMessage(String message, String source) {
  // Disable receive interrupt during transmit
  enableInterrupt = false;
  
  // Small delay to ensure radio is ready
  delay(50);
  
  // Show sending status
  Heltec.display->clear();
  Heltec.display->setTextAlignment(TEXT_ALIGN_CENTER);
  Heltec.display->setFont(ArialMT_Plain_16);
  Heltec.display->drawString(64, 20, "SENDING...");
  Heltec.display->display();
  
  Serial.println("Switching to TX mode...");
  
  // Create packet with header - use 3 bytes for ID
  char idStr[8];
  sprintf(idStr, "%06X", (uint32_t)(devId & 0xFFFFFF));
  String packet = "MSG:" + String(idStr) + ":" + message + ":END";
  
  // Transmit packet
  int16_t st = radio.transmit(packet);
  
  if (st == RADIOLIB_ERR_NONE) {
    Serial.println("TX SUCCESS: " + message);
    txCount++;
    lastTxMsg = message;  // Store the sent message
    lastMsgSource = source;
    
    // Show success
    Heltec.display->clear();
    Heltec.display->drawString(64, 20, "SENT!");
    Heltec.display->display();
    delay(300);
  } else {
    Serial.printf("TX ERROR: %d\n", st);
    Heltec.display->clear();
    Heltec.display->drawString(64, 20, "TX ERROR!");
    Heltec.display->display();
    delay(500);
  }
  
  // Clear any pending interrupts
  receivedFlag = false;
  
  // Return to receive mode with interrupt
  Serial.println("Switching back to RX mode...");
  radio.clearDio1Action();
  delay(10);
  radio.setDio1Action(setFlag);
  
  st = radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("Failed to start receive mode: %d\n", st);
  } else {
    Serial.println("RX mode active");
  }
  
  // Re-enable interrupt
  enableInterrupt = true;
  
  updateDisplay();
}

void sendBLENotification(String message) {
  if (deviceConnected && pTxCharacteristic) {
    // iOS has issues with large notifications, keep them small
    // Maximum safe size for iOS is 20 bytes per notification
    const int maxChunkSize = 20;
    
    if (message.length() <= maxChunkSize) {
      pTxCharacteristic->setValue(message.c_str());
      pTxCharacteristic->notify();
      Serial.println("BLE TX: " + message);
    } else {
      // For longer messages, truncate to avoid iOS issues
      String truncated = message.substring(0, maxChunkSize);
      pTxCharacteristic->setValue(truncated.c_str());
      pTxCharacteristic->notify();
      Serial.println("BLE TX (truncated): " + truncated);
    }
  }
}

void IRAM_ATTR buttonISR() {
  buttonPressed = true;
}

void initBLE() {
  // Create unique device name based on MAC address
  // Use last 3 bytes (6 hex chars) for better uniqueness
  uint32_t uniqueId = (uint32_t)(devId & 0xFFFFFF);
  char hexStr[8];
  sprintf(hexStr, "%06X", uniqueId);  // Ensure 6 digits with leading zeros
  String deviceName = "LoRa_" + String(hexStr);
  
  Serial.println("Starting BLE with name: " + deviceName);
  Serial.printf("BLE Name Debug - MAC: %012llX, UniqueID: %06X, Name: %s\n", 
                devId, uniqueId, deviceName.c_str());
  
  // Create the BLE Device
  BLEDevice::init(deviceName.c_str());
  // Don't set MTU - let iOS handle it
  // BLEDevice::setMTU(185); // REMOVED - iOS doesn't like this

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create BLE Characteristics
  // TX Characteristic (we send notifications to the app)
  pTxCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID_TX,
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
                      
  pTxCharacteristic->addDescriptor(new BLE2902());

  // RX Characteristic (we receive data from the app)
  // iOS prefers WRITE_NR (Write Without Response)
  BLECharacteristic * pRxCharacteristic = pService->createCharacteristic(
                                          CHARACTERISTIC_UUID_RX,
                                          BLECharacteristic::PROPERTY_WRITE | 
                                          BLECharacteristic::PROPERTY_WRITE_NR
                                        );

  pRxCharacteristic->setCallbacks(new MyCallbacks());

  // Start the service
  pService->start();

  // Start advertising with proper settings for iOS
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);  // iOS sometimes has issues with scan response
  pAdvertising->setMinPreferred(0x0);    // Let iOS decide connection parameters
  BLEDevice::startAdvertising();
  
  Serial.println("BLE Nordic UART Service started, waiting for connections...");
}

void setup() {
  // Initialize Heltec board with display enabled, LoRa disabled (we'll init it manually), Serial enabled
  Heltec.begin(true /*DisplayEnable*/, false /*LoRa Enable*/, true /*Serial Enable*/);
  
  delay(300);
  Serial.println("\n\n=== Heltec LoRa Chat with BLE Bridge Starting ===");
  
  // Get unique ID from MAC address
  devId = ESP.getEfuseMac();
  Serial.printf("Full MAC: %012llX\n", devId);
  Serial.printf("Device ID: %06X\n", (uint32_t)(devId & 0xFFFFFF));  // Show 6 hex digits
  
  // Initialize BLE
  initBLE();
  
  // Initialize button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
  
  // Show startup message
  Heltec.display->clear();
  Heltec.display->setTextAlignment(TEXT_ALIGN_CENTER);
  Heltec.display->setFont(ArialMT_Plain_16);
  Heltec.display->drawString(64, 20, "STARTING");
  Heltec.display->display();
  delay(1000);
  
  // Initialize SPI for LoRa
  SPI.begin(9, 11, 10, 8);  // SCK, MISO, MOSI, CS
  
  // Initialize LoRa radio
  Serial.println("Initializing LoRa radio...");
  Serial.printf("[SX1262] Initializing ... ");
  int state = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR_DEN, 0x12, PWR_DBM, 8);
  
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("failed, code %d\n", state);
    Heltec.display->clear();
    Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
    Heltec.display->setFont(ArialMT_Plain_10);
    Heltec.display->drawString(0, 20, "Radio Error: " + String(state));
    Heltec.display->display();
    while (true);
  }
  Serial.println("success!");
  
  // Set additional radio parameters
  radio.setCRC(2);  // CRC enabled
  radio.fixedPacketLengthMode(0); // Variable length packets
  radio.setPreambleLength(16);  // Increase preamble for better detection
  
  // Set the function that will be called when packet is received
  radio.setDio1Action(setFlag);
  
  // Start listening for LoRa packets
  Serial.print("[SX1262] Starting to listen ... ");
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("failed, code %d\n", state);
    while (true);
  }
  Serial.println("success!");
  
  Serial.printf("LoRa ready @ %.1f MHz, SF%d/BW%.0fk/CR4/%d\n", FREQ_MHZ, SF, BW_KHZ, CR_DEN);
  char idStr[8];
  sprintf(idStr, "%06X", (uint32_t)(devId & 0xFFFFFF));
  Serial.println("BLE Device Name: LoRa_" + String(idStr));
  Serial.println("Press button or send BLE message!");
  Serial.println("=================================\n");
  
  updateDisplay();
}

void loop() {
  // Handle BLE connection changes
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); // give the bluetooth stack the chance to get things ready
    pServer->startAdvertising(); // restart advertising
    Serial.println("Start advertising");
    oldDeviceConnected = deviceConnected;
    updateDisplay();
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
    updateDisplay();
  }
  
  // Print debug info every 5 seconds
  if (millis() - lastDebugPrint > 5000) {
    lastDebugPrint = millis();
    Serial.printf("Status - TX: %lu, RX: %lu, BLE: %s\n", 
                  txCount, rxCount, deviceConnected ? "Connected" : "Disconnected");
  }
  
  // Check for BLE message to send
  if (hasBleMessage && bleMessageToSend.length() > 0) {
    hasBleMessage = false;
    sendMessage(bleMessageToSend, "BLE");
    bleMessageToSend = "";
  }
  
  // Check for button press
  if (buttonPressed && (millis() - lastButtonPress > DEBOUNCE_DELAY)) {
    buttonPressed = false;
    lastButtonPress = millis();
    
    // Create a fun message based on message count
    msgCount++;
    char idStr[8];
    sprintf(idStr, "%06X", (uint32_t)(devId & 0xFFFFFF));
    String messages[] = {
      "Hello from " + String(idStr) + "!",
      "Button pressed! Count: " + String(msgCount),
      "LoRa is awesome!",
      "Can you hear me?",
      "Testing 1-2-3...",
      "Weather is nice today!",
      "Coffee break time?",
      "ESP32-S3 rocks!"
    };
    
    String msg = messages[msgCount % 8];
    sendMessage(msg, "BTN");
  }
  
  // Check if we received a packet
  if (receivedFlag && enableInterrupt) {
    // Disable interrupt during processing
    enableInterrupt = false;
    receivedFlag = false;
    
    String str;
    int state = radio.readData(str);
    
    if (state == RADIOLIB_ERR_NONE) {
      // Parse packet - expecting format: MSG:ID:message:END
      if (str.startsWith("MSG:") && str.endsWith(":END")) {
        // Remove the MSG: prefix and :END suffix
        str = str.substring(4, str.length() - 4);
        
        // Find the first colon (after the ID)
        int colonPos = str.indexOf(':');
        
        if (colonPos > 0) {
          String senderId = str.substring(0, colonPos);
          String message = str.substring(colonPos + 1);
          
          // Successfully parsed message
          rxCount++;
          lastRSSI = radio.getRSSI();
          lastSNR = radio.getSNR();
          lastRxMsg = message;
          
          Serial.println("=== MESSAGE RECEIVED ===");
          Serial.println("From: " + senderId);
          Serial.println("Data: " + message);
          Serial.printf("RSSI: %.2f dBm\n", lastRSSI);
          Serial.printf("SNR: %.2f dB\n", lastSNR);
          Serial.println("=======================");
          
          // Send to BLE if connected
          // Keep it simple for iOS - just sender and message
          String bleMsg = senderId + ": " + message;
          sendBLENotification(bleMsg);
          
          // Flash the display
          Heltec.display->invertDisplay();
          delay(100);
          Heltec.display->normalDisplay();
          
          updateDisplay();
        } else {
          Serial.println("Malformed packet structure - no ID separator");
        }
      } else {
        // Try to handle partial or corrupted packets
        Serial.println("Invalid packet format: " + str);
        Serial.println("Length: " + String(str.length()));
        Serial.println("Hex dump:");
        for (int i = 0; i < str.length(); i++) {
          Serial.printf("%02X ", (uint8_t)str[i]);
          if ((i + 1) % 16 == 0) Serial.println();
        }
        Serial.println();
      }
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("[SX1262] CRC error!");
    } else {
      Serial.printf("[SX1262] Failed, code %d\n", state);
    }
    
    // Put radio back to receive mode
    radio.startReceive();
    
    // Re-enable interrupt
    enableInterrupt = true;
  }
}
