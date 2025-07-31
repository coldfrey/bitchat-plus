#pragma once

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>
#include <NimBLEAdvertising.h>
#include "bitchat_protocol.h"

class BLEMesh {
public:
    static void init();
    static void process();
    static String getPeerID();
    static void sendData(const uint8_t* data, size_t length, const String& targetPeerID = "");
    
private:
    // Server (Peripheral) components
    static NimBLEServer* pServer;
    static NimBLEService* pService;
    static NimBLECharacteristic* pCharacteristic;
    static NimBLEAdvertising* pAdvertising;
    
    // Peer ID management
    static String myPeerID;
    static void generatePeerID();
    
    // BLE Server callbacks
    static void startAdvertising();
    static void onConnect(NimBLEServer* pServer);
    static void onDisconnect(NimBLEServer* pServer);
    static void onWrite(NimBLECharacteristic* pCharacteristic);
    
public:
    // Message handling  
    static void handleReceivedData(const uint8_t* data, size_t length);

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