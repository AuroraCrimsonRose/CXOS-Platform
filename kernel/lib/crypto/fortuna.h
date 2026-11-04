// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/crypto/fortuna.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The kernel's generator, without the kernel: pure state and computation, so
 * the host tests run it (drivers/char/random.c gives it its samples, its lock
 * and its clock). docs/security/CX_RANDOM.md is the design.
 *
 * Two phases:
 *   - Unseeded: everything added goes into one hash, `boot`, with the bits
 *     its caller credits it. At FORTUNA_SEED_BITS the key becomes
 *     SHA-256(key || SHA-256(boot)) and the generator is seeded. Nothing is
 *     generated before that: there is no fallback seed.
 *   - Seeded: Fortuna (Ferguson and Schneier). What is added goes round the
 *     32 pools in turn, uncredited. A request reseeds first when pool 0 has
 *     taken FORTUNA_POOL0_MIN bytes and FORTUNA_RESEED_NS has passed since the
 *     last; reseed r (counting from 1) takes pool i when 2^i divides r, so
 *     pool i is drained every 2^i reseeds and an attacker who sees or floods
 *     most samples still loses to a pool that waited long enough.
 *
 * Output is ChaCha20 keystream with fast key erasure (Bernstein): the first 32
 * bytes of each request's keystream become the next key before any byte is
 * returned, so a key read out of memory later says nothing about what was
 * already handed out.
 */

#ifndef FORTUNA_H
#define FORTUNA_H

#include <stdint.h>
#include "sha256.h"
#include "chacha20.h"

#define FORTUNA_POOLS       32
#define FORTUNA_SEED_BITS   256
#define FORTUNA_POOL0_MIN   64                  /* bytes, as the book has it */
#define FORTUNA_RESEED_NS   100000000ull        /* 100 ms, as the book has it */

struct fortuna {
    uint8_t  key[CHACHA20_KEY_SIZE];
    uint32_t seeded;
    uint32_t credit;               /* bits credited to `boot`, until seeded */
    struct sha256_ctx boot;
    struct sha256_ctx pool[FORTUNA_POOLS];
    uint32_t pool0_bytes;          /* added to pool 0 since its last drain */
    uint32_t next_pool;
    uint64_t reseeds;              /* r: reseeds done, the seeding not counted */
    uint64_t last_reseed_ns;
};

void fortuna_init(struct fortuna *f);

/* Add `len` bytes of an event, crediting `bits` while unseeded (ignored
   after: the pools need no estimate). Seeds the generator once the credit
   reaches FORTUNA_SEED_BITS. */
void fortuna_add(struct fortuna *f, const void *data, uint32_t len, uint32_t bits);

/* `len` bytes of output into `out`: 0, or -1 while unseeded (nothing
   written). `now_ns` is the monotonic clock, for the reseed interval. */
int fortuna_generate(struct fortuna *f, void *out, uint32_t len, uint64_t now_ns);

/* Which pools reseed r (from 1) drains: bit i for pool i. */
uint32_t fortuna_reseed_pools(uint64_t r);

/* ---- what a sample is worth ----
   Timing samples (the cycle counter at an interrupt, or after a loop at
   boot) are credited at most one bit each, as XNU assumes for its samples
   (osfmk/prng/entropy.c, entropy_provide), and none unless the first,
   second and third differences of the stream all reach JITTER_MIN_CYCLES.
   A counter that moves in fixed steps, stuck, or in steps that only crawl
   (QEMU's -icount, where the counter is a function of instructions run)
   is credited nothing. */
#define JITTER_MIN_CYCLES 16u

struct jitter_est {
    uint32_t last, d1, d2;          /* the last sample and its last differences */
    uint32_t n;                     /* samples seen, to 3 */
};

/* 1 if `sample` earns a bit, else 0; then remembers it. */
uint32_t jitter_credit(struct jitter_est *e, uint32_t sample);

/* Whether a word from RDSEED or RDRAND may be credited: not 0, not all ones
   (some AMD parts returned all ones after a resume while reporting success),
   and not the word the instruction gave last time. */
int rng_word_plausible(uint64_t v, uint64_t prev);

#endif
