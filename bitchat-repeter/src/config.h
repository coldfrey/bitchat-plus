#pragma once
#include <stdint.h>

struct GwConfig {
    uint32_t gw_id;          // random on first boot
    uint8_t  region;         // 0=EU868,1=US915,2=AS923...
    uint8_t  sf;             // 7..12
    uint8_t  bw;             // 0=125k,1=250k,2=500k
    int8_t   tx_dbm;         // within legal bounds
    uint8_t  ttl;            // default 6
    uint8_t  ble_adv_int_ms; // e.g., 200
    uint8_t  log_level;      // 0..3
    uint8_t  _resv;
    uint8_t  gw_secret[16];  // for EID
} __attribute__((packed));

namespace Config {
    void init();
    const GwConfig& cur();
    void save();
    void updateLiveParams(uint8_t sf, uint8_t bw, int8_t tx_dbm, uint8_t ttl);
}