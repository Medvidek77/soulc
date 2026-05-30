#ifndef MD5_H
#define MD5_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[64];
} MD5_CTX;

void MD5_Init(MD5_CTX *context);
void MD5_Update(MD5_CTX *context, const void *data, size_t len);
void MD5_Final(uint8_t digest[16], MD5_CTX *context);

#endif
