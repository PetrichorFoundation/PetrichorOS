/* SHA-256, shared by the host repo tool and the OS (no libc dependencies). */
#pragma once
#include <stddef.h>
#include <stdint.h>

struct sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint32_t bits_hi, bits_lo;
    size_t fill;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, uint8_t out[32]);
void sha256_hex(const void *data, size_t len, char out[65]);     /* one-shot, NUL-terminated hex */
