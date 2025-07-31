#pragma once

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>
#include <NimBLEAdvertising.h>
#include <map>
#include "bitchat_protocol.h"

// Connection state for each iOS device
struct ConnectionState {
    String peerID;              // iOS device peer ID
    uint8_t negotiatedVersion;  // Agreed protocol version (0 = not negotiated)
    bool isReady;              // True after successful version negotiation
    unsigned long connectTime; // When the connection was established
    
    ConnectionState() : negotiatedVersion(0), isReady(false), connectTime(0) {}
};

class BLEMesh {
public:
    static void init();
    static void process();
    static String getPeerID();
    static void sendData(const uint8_t* data, size_t length);
    static int getConnectedClientCount();
    
private:
    // Server (Peripheral) components - for iOS device connections only
    static NimBLEServer* pServer;
    static NimBLEService* pService;
    static NimBLECharacteristic* pCharacteristic;
    static NimBLEAdvertising* pAdvertising;
    
    // Peer ID management
    static String myPeerID;
    static void generatePeerID();
    
    // Connection state management
    static std::map<uint16_t, ConnectionState> connectionStates; // Map connection handle to state
    
    // Version negotiation
    static void handleVersionHello(const uint8_t* data, size_t length, uint16_t connectionHandle);
    static void sendVersionAck(uint8_t agreedVersion, uint16_t connectionHandle);
    static bool isConnectionReady(uint16_t connectionHandle);
    
    // Message type handlers
    static void handleAnnounceMessage(const BitchatPacket& packet, uint16_t connectionHandle);
    static void handleLeaveMessage(const BitchatPacket& packet, uint16_t connectionHandle);
    static void handleChatMessage(const BitchatPacket& packet, uint16_t connectionHandle);
    static void handleAckMessage(const BitchatPacket& packet, uint16_t connectionHandle);
    
    // BLE Server callbacks
    static void startAdvertising();
    
public:
    // Message handling  
    static void handleReceivedData(const uint8_t* data, size_t length, uint16_t connectionHandle);

private:
};

// Server callback class
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) override;
    void onDisconnect(NimBLEServer* pServer) override;
};

// Characteristic callback class  
class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) override;
};