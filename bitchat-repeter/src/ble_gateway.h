#pragma once

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>
#include <NimBLEAdvertising.h>
#include <map>
#include "bitchat_protocol.h"

// iOS device connection state
struct iOSConnectionState {
    String deviceID;            // iOS device identifier  
    uint8_t negotiatedVersion;  // Agreed protocol version (0 = not negotiated)
    bool isReady;              // True after successful version negotiation
    unsigned long connectTime; // When the connection was established
    
    iOSConnectionState() : negotiatedVersion(0), isReady(false), connectTime(0) {}
};

/**
 * BLE Gateway - Provides BLE GATT server functionality for iOS devices
 * to connect to the BitChat LoRa mesh network.
 * 
 * This class acts as a bridge between iOS BitChat apps (via BLE) and the
 * LoRa mesh network, handling protocol negotiation, message forwarding,
 * and connection management.
 */
class BLEGateway {
    // Friend classes for BLE callback access
    friend class ServerCallbacks;
    friend class CharacteristicCallbacks;
    
public:
    static void init();
    static void process();
    static String getGatewayID();
    static void sendToiOSDevices(const uint8_t* data, size_t length);
    static int getConnectedDeviceCount();
    static void setAdvertisingInterval(uint32_t intervalMs);
    
private:
    // BLE GATT Server components for iOS device connections
    static NimBLEServer* pServer;
    static NimBLEService* pService;
    static NimBLECharacteristic* pCharacteristic;
    static NimBLEAdvertising* pAdvertising;
    
    // Gateway identification
    static String gatewayID;
    static void generateGatewayID();
    
    // iOS device connection management
    static std::map<uint16_t, iOSConnectionState> connectionStates;
    static void setConnectionState(uint16_t connectionHandle, const iOSConnectionState& state);
    
    // Protocol version negotiation with iOS devices
    static void handleVersionHello(const uint8_t* data, size_t length, uint16_t connectionHandle);
    static void sendVersionAck(uint8_t agreedVersion, uint16_t connectionHandle);
    static bool isConnectionReady(uint16_t connectionHandle);
    
    // BitChat message handlers (for logging/debugging purposes)
    static void handleAnnounceMessage(const BitchatPacket& packet, uint16_t connectionHandle);
    static void handleLeaveMessage(const BitchatPacket& packet, uint16_t connectionHandle);
    static void handleChatMessage(const BitchatPacket& packet, uint16_t connectionHandle);
    static void handleAckMessage(const BitchatPacket& packet, uint16_t connectionHandle);
    
    // BLE advertising
    static void startAdvertising();
    
public:
    // Message processing from iOS devices
    static void handleReceivedData(const uint8_t* data, size_t length, uint16_t connectionHandle);
};

// BLE Server callback handlers
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) override;
    void onDisconnect(NimBLEServer* pServer) override;
};

// BLE Characteristic callback handlers
class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) override;
};