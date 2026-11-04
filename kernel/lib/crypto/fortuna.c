// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/crypto/fortuna.c */
/* Aurora Tejeda / CATX Systems */
/* The generator and its pools (fortuna.h says how). */

#include "fortuna.h"

static void wipe(void *p, uint32_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

void fortuna_init(struct fortuna *f) {
    wipe(f, sizeof *f);
    sha256_init(&f->boot);
    for (int i = 0; i < FORTUNA_POOLS; i++) sha256_init(&f->pool[i]);
}

/* Seeding and reseeding both make key = SHA-256(key || digests): the old key
   goes into the new one, so a seed that adds little takes nothing away. */

void fortuna_add(struct fortuna *f, const void *data, uint32_t len, uint32_t bits) {
    if (!f->seeded) {
        sha256_update(&f->boot, data, len);
        f->credit += bits;
        if (f->credit < FORTUNA_SEED_BITS) return;
        uint8_t d[SHA256_DIGEST_SIZE];
        sha256_final(&f->boot, d);
        struct sha256_ctx k;
        sha256_init(&k);
        sha256_update(&k, f->key, sizeof f->key);
        sha256_update(&k, d, sizeof d);
        sha256_final(&k, f->key);
        wipe(d, sizeof d);
        wipe(&k, sizeof k);
        sha256_init(&f->boot);
        f->seeded = 1;
        return;
    }
    uint32_t p = f->next_pool;
    f->next_pool = (p + 1) % FORTUNA_POOLS;
    sha256_update(&f->pool[p], data, len);
    if (p == 0) f->pool0_bytes += len;
}

uint32_t fortuna_reseed_pools(uint64_t r) {
    uint32_t m = 0;
    for (int i = 0; i < FORTUNA_POOLS; i++) {
        if (r & (((uint64_t)1 << i) - 1)) break;   /* 2^i does not divide r */
        m |= 1u << i;
    }
    return m;
}

static void reseed(struct fortuna *f, uint64_t now_ns) {
    f->reseeds++;
    uint32_t m = fortuna_reseed_pools(f->reseeds);
    struct sha256_ctx k;
    uint8_t d[SHA256_DIGEST_SIZE];
    sha256_init(&k);
    sha256_update(&k, f->key, sizeof f->key);
    for (int i = 0; i < FORTUNA_POOLS; i++) {
        if (!(m & (1u << i))) continue;
        sha256_final(&f->pool[i], d);
        sha256_init(&f->pool[i]);
        sha256_update(&k, d, sizeof d);
    }
    sha256_final(&k, f->key);
    wipe(d, sizeof d);
    wipe(&k, sizeof k);
    f->pool0_bytes = 0;
    f->last_reseed_ns = now_ns;
}

int fortuna_generate(struct fortuna *f, void *out, uint32_t len, uint64_t now_ns) {
    if (!f->seeded) return -1;
    if (f->pool0_bytes >= FORTUNA_POOL0_MIN && now_ns - f->last_reseed_ns >= FORTUNA_RESEED_NS)
        reseed(f, now_ns);

    /* Each request has a key of its own, so the nonce can stay 0. */
    static const uint8_t nonce[12] = { 0 };
    uint8_t blk[CHACHA20_BLOCK_SIZE], next[CHACHA20_KEY_SIZE];
    uint8_t *o = (uint8_t *)out;
    uint32_t ctr = 0;

    chacha20_block(f->key, ctr++, nonce, blk);
    for (uint32_t i = 0; i < CHACHA20_KEY_SIZE; i++) next[i] = blk[i];
    uint32_t have = CHACHA20_BLOCK_SIZE - CHACHA20_KEY_SIZE, at = CHACHA20_KEY_SIZE;
    while (len) {
        if (!have) { chacha20_block(f->key, ctr++, nonce, blk); have = CHACHA20_BLOCK_SIZE; at = 0; }
        uint32_t n = len < have ? len : have;
        for (uint32_t i = 0; i < n; i++) *o++ = blk[at + i];
        at += n; have -= n; len -= n;
    }
    for (uint32_t i = 0; i < CHACHA20_KEY_SIZE; i++) f->key[i] = next[i];
    wipe(blk, sizeof blk);
    wipe(next, sizeof next);
    return 0;
}

static uint32_t absdiff(uint32_t a, uint32_t b) {
    uint32_t d = a - b;
    return (int32_t)d < 0 ? (uint32_t)0 - d : d;
}

uint32_t jitter_credit(struct jitter_est *e, uint32_t sample) {
    uint32_t d1 = sample - e->last;              /* modulo 2^32: the counter wraps */
    uint32_t d2 = absdiff(d1, e->d1);
    uint32_t d3 = absdiff(d2, e->d2);
    uint32_t seen = e->n;
    e->last = sample; e->d1 = d1; e->d2 = d2;
    if (seen < 3) { e->n = seen + 1; return 0; }
    return d1 >= JITTER_MIN_CYCLES && d2 >= JITTER_MIN_CYCLES && d3 >= JITTER_MIN_CYCLES;
}

int rng_word_plausible(uint64_t v, uint64_t prev) {
    return v != 0 && v != ~(uint64_t)0 && v != prev;
}
