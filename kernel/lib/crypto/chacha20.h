// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/crypto/chacha20.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The ChaCha20 block function (RFC 8439 section 2.3): a 256-bit key, a
 * 32-bit block counter and a 96-bit nonce give 64 bytes of keystream. Pure
 * computation, like sha256.c; the kernel's generator (fortuna.c) is its only
 * user. No cipher API: the generator wants keystream, never encryption.
 */

#ifndef CHACHA20_H
#define CHACHA20_H

#include <stdint.h>

#define CHACHA20_KEY_SIZE   32
#define CHACHA20_BLOCK_SIZE 64

/* Keystream block `counter` for `key` and `nonce` (12 bytes), into out[64].
   The key and nonce are read as little-endian words, as the RFC says. */
void chacha20_block(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[CHACHA20_BLOCK_SIZE]);

#endif
