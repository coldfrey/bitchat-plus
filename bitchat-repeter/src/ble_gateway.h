#pragma once
#include "config.h"
#include "../include/bridge.h"

// Gateway Service UUIDs
#define GATEWAY_SERVICE_UUID     "7A1B0000-6B2E-46E8-8D2D-6AA2E5A4F001"
#define GW_CHAR_TX_UUID         "7A1B1001-6B2E-46E8-8D2D-6AA2E5A4F001"  // WRITE_NR (Phone->GW)
#define GW_CHAR_RX_UUID         "7A1B1002-6B2E-46E8-8D2D-6AA2E5A4F001"  // NOTIFY (GW->Phone)
#define GW_CHAR_CFG_UUID        "7A1B2001-6B2E-46E8-8D2D-6AA2E5A4F001"  // READ/WRITE
#define GW_CHAR_STATS_UUID      "7A1B2002-6B2E-46E8-8D2D-6AA2E5A4F001"  // READ/NOTIFY
#define BLE_AD_SERVICE_UUID     GATEWAY_SERVICE_UUID

namespace BLEGateway {
    void init(const GwConfig& cfg);
    void notify(const Frame& f);
    void updateAdvertisement();
    void startAdvertising();
}