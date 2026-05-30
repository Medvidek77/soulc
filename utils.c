#include "utils.h"

void put_u32_le(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

uint32_t get_u32_le(const uint8_t *buf) {
    return ((uint32_t)buf[0]) |
           (((uint32_t)buf[1]) << 8) |
           (((uint32_t)buf[2]) << 16) |
           (((uint32_t)buf[3]) << 24);
}
