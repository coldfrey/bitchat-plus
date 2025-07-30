#pragma once
#include <stdint.h>

struct GwStats {
    uint32_t uptime_s;
    uint32_t lora_rx;
    uint32_t lora_tx;
    uint32_t ble_rx;
    uint32_t ble_tx;
    uint32_t dedup_hits;
    uint32_t crc_err;
    uint32_t duty_block_ms;
} __attribute__((packed));

namespace Stats {
    extern uint32_t lora_rx;
    extern uint32_t lora_tx;
    extern uint32_t ble_rx;
    extern uint32_t ble_tx;
    extern uint32_t dedup_hits;
    extern uint32_t crc_err;
    extern uint32_t duty_block_ms;
    
    void init();
    GwStats getSnapshot();
    void print();
}