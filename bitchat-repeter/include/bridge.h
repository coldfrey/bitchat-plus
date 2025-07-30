#pragma once
#include <stdint.h>

#define BRIDGE_MAGIC 0xBC77

#pragma pack(push,1)
struct BridgeHdr {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  ttl;
    uint64_t msg_id;
    uint32_t gw_id;
    uint8_t  frag_idx;
    uint8_t  frag_total;
    uint16_t payload_len;
    uint16_t hop;
    uint16_t crc16;
};
#pragma pack(pop)

struct Frame {
    BridgeHdr hdr;
    uint16_t  len;
    uint8_t   data[228]; // adjust to match LoRa payload cap
};