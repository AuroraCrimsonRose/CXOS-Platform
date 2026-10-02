// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/sha256.h */
/* Aurora Tejeda / CATX Systems */
/*
 * SHA-256 (FIPS 180-4). Pure computation - no kernel dependencies (no heap,
 * no interrupts, no allocation). Used as the integrity hash for CXOS code
 * signing (see CX_EXTENSION_SYSTEM.md section 10): an artifact is hashed with
 * SHA-256, and that digest is what an RSA signature is created/verified over.
 *
 * Two APIs:
 *   - one-shot:  sha256(data, len, out)         hash a whole buffer
 *   - streaming: sha256_init / _update / _final  hash data in pieces (needed
 *                for hashing a large artifact read block-by-block from disk)
 */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_SIZE 32      /* 256 bits */
#define SHA256_BLOCK_SIZE  64      /* 512-bit input block */

struct sha256_ctx {
    uint32_t state[8];             /* the eight working hash words */
    uint64_t bitlen;               /* total message length in BITS */
    uint8_t  buf[SHA256_BLOCK_SIZE];
    uint32_t buflen;               /* bytes currently buffered (< 64) */
};

/* streaming */
void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, uint8_t out[SHA256_DIGEST_SIZE]);

/* one-shot convenience: hash `len` bytes of `data` into out[32]. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

#endif