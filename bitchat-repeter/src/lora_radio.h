#pragma once
#include "config.h"
#include "../include/bridge.h"

namespace LoRaRadio {
    void init(const GwConfig& cfg);
    void send(const Frame& f);
    void startReceiveTask();
    bool updateParams(uint8_t sf, uint8_t bw, int8_t tx_dbm);
}