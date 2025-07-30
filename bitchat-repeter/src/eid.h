#pragma once
#include <stdint.h>
#include <cstddef>  // For size_t
#include "config.h"

namespace EID {
    void init(const GwConfig& cfg);
    void fillServiceData(uint8_t* data, size_t max_len);
    void startRotationTask();
}