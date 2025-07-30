#pragma once
#include "../include/bridge.h"

namespace Router {
    void init();
    void onBleFrame(const Frame& f);
    void onLoRaFrame(const Frame& f);
}