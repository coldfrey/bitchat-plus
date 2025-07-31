#pragma once

// Heltec WiFi LoRa 32 V3 Pin Definitions
// LoRa SX1262 pins
#define LORA_SCK    9
#define LORA_MISO   11
#define LORA_MOSI   10
#define LORA_CS     8
#define LORA_RST    12
#define LORA_DIO1   14
#define LORA_BUSY   13
#define LORA_TXEN   -1  // Not used on V3
#define LORA_RXEN   -1  // Not used on V3

// OLED Display pins (optional)
#define OLED_SDA    17
#define OLED_SCL    18
#define OLED_RST    21

// LED and Button
#define LED_PIN     35
#define BUTTON_PIN  0

// Battery ADC
#define BATTERY_ADC 1  // ADC1_CH0, GPIO1

// Power control
#define VEXT_ENABLE 36  // Low to enable Vext