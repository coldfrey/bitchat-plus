#pragma once

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>
#include <NimBLEAdvertising.h>
#include <NimBLEScan.h>
#include <NimBLEClient.h>
#include <map>
#include "bitchat_protocol.h"

struct ConnectedPeer {
    NimBLEClient* client;
    NimBLERemoteCharacteristic* characteristic;
    String peerID;
    unsigned long lastSeen;
    bool isReady;
};

class BLEMesh {
public:
    static void init();
    static void process();
    static String getPeerID();
    static void sendData(const uint8_t* data, size_t length, const String& targetPeerID = "");
    static int getConnectedPeerCount();
    
private:
    // Server (Peripheral) components
    static NimBLEServer* pServer;
    static NimBLEService* pService;
    static NimBLECharacteristic* pCharacteristic;
    static NimBLEAdvertising* pAdvertising;
    
    // Client (Central) components
    static NimBLEScan* pScan;
    static std::map<String, ConnectedPeer> connectedPeers; // address -> peer info
    static const int MAX_CONNECTIONS = 3;
    
    // Peer ID management
    static String myPeerID;
    static void generatePeerID();
    
    // BLE Server callbacks
    static void startAdvertising();
    static void onConnect(NimBLEServer* pServer);
    static void onDisconnect(NimBLEServer* pServer);
    static void onWrite(NimBLECharacteristic* pCharacteristic);
    
public:
    // BLE Client functions
    static void startScanning();
    static void stopScanning();
    static bool connectToPeer(NimBLEAdvertisedDevice* device);
    static void disconnectPeer(const String& address);
    static void cleanupDisconnectedPeers();

private:
    
public:
    // Message handling  
    static void handleReceivedData(const uint8_t* data, size_t length, const String& sourcePeerID = "");

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

// Static callback functions for NimBLE
class BLECallbacks {
public:
    static void scanResult(NimBLEAdvertisedDevice* advertisedDevice);
    static void clientConnect(NimBLEClient* pClient);
    static void clientDisconnect(NimBLEClient* pClient);
    static void characteristicNotify(NimBLERemoteCharacteristic* pBLERemoteCharacteristic, 
                                   uint8_t* pData, size_t length, bool isNotify);
};