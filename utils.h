#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>

void put_u32_le(uint8_t *buf, uint32_t val);
uint32_t get_u32_le(const uint8_t *buf);

#endif
