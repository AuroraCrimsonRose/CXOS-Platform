// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/crypto/chacha20.c */
/* Aurora Tejeda / CATX Systems */
/* The ChaCha20 block function, RFC 8439 sections 2.1-2.3. */

#include "chacha20.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t rotl(uint32_t v, int n) { return v << n | v >> (32 - n); }

#define QR(a, b, c, d)                       \
    a += b; d ^= a; d = rotl(d, 16);         \
    c += d; b ^= c; b = rotl(b, 12);         \
    a += b; d ^= a; d = rotl(d, 8);          \
    c += d; b ^= c; b = rotl(b, 7)

void chacha20_block(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[CHACHA20_BLOCK_SIZE]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu; s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++) s[4 + i] = rd32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = rd32(nonce + 4 * i);
    for (int i = 0; i < 16; i++) x[i] = s[i];

    for (int r = 0; r < 10; r++) {           /* 20 rounds: column then diagonal */
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];
        out[4 * i]     = (uint8_t)v;
        out[4 * i + 1] = (uint8_t)(v >> 8);
        out[4 * i + 2] = (uint8_t)(v >> 16);
        out[4 * i + 3] = (uint8_t)(v >> 24);
    }
    /* The state holds the key: leave none of it on the stack. */
    for (int i = 0; i < 16; i++) { ((volatile uint32_t *)s)[i] = 0; ((volatile uint32_t *)x)[i] = 0; }
}
