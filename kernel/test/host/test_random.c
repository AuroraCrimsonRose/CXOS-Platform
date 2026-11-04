// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_random.c */
/* Aurora Tejeda / CATX Systems */
/*
 * lib/crypto/chacha20.c and fortuna.c: the kernel's generator.
 *
 * The ChaCha20 blocks are RFC 8439's own (section 2.3.2, and appendix A.1
 * test vector 1), not values this code produced. The rest pins the rules
 * fortuna.h states: nothing before 256 credited bits, the reseed schedule,
 * fast key erasure, and what the estimator refuses to credit.
 */
#include "khost.h"
#include "lib/crypto/chacha20.h"
#include "lib/crypto/fortuna.h"

#include <stdlib.h>
#include <string.h>

static int bytes_are(const uint8_t *d, const char *hex, int n) {
    for (int i = 0; i < n; i++) {
        char pair[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        if (d[i] != (uint8_t)strtoul(pair, NULL, 16)) return 0;
    }
    return 1;
}

KH_TEST(chacha20_matches_rfc_8439) {
    uint8_t key[32], nonce[12] = { 0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0 }, out[64];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    chacha20_block(key, 1, nonce, out);
    KH_CHECK(bytes_are(out,
        "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", 64));

    memset(key, 0, sizeof key);
    memset(nonce, 0, sizeof nonce);
    chacha20_block(key, 0, nonce, out);
    KH_CHECK(bytes_are(out,
        "76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b770dc7"
        "da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669b2ee6586", 64));
}

KH_TEST(fortuna_reseed_r_takes_pool_i_when_2_to_the_i_divides_r) {
    KH_CHECK_EQ(fortuna_reseed_pools(1), 0x1u);
    KH_CHECK_EQ(fortuna_reseed_pools(2), 0x3u);
    KH_CHECK_EQ(fortuna_reseed_pools(3), 0x1u);
    KH_CHECK_EQ(fortuna_reseed_pools(4), 0x7u);
    KH_CHECK_EQ(fortuna_reseed_pools(6), 0x3u);
    KH_CHECK_EQ(fortuna_reseed_pools(96), 0x3Fu);
    KH_CHECK_EQ(fortuna_reseed_pools((uint64_t)1 << 31), 0xFFFFFFFFu);
    KH_CHECK_EQ(fortuna_reseed_pools((uint64_t)1 << 40), 0xFFFFFFFFu);
}

/* Seeded from `bits` credited bits of a fixed event. */
static void seed(struct fortuna *f, uint32_t bits) {
    fortuna_init(f);
    const char ev[] = "an event";
    fortuna_add(f, ev, sizeof ev, bits);
}

KH_TEST(fortuna_generates_nothing_before_256_credited_bits) {
    struct fortuna f;
    uint8_t out[16];
    fortuna_init(&f);
    memset(out, 0xA5, sizeof out);
    KH_CHECK_EQ(fortuna_generate(&f, out, sizeof out, 0), -1);
    for (int i = 0; i < 16; i++) KH_CHECK_EQ(out[i], 0xA5);

    for (int i = 0; i < 255; i++) fortuna_add(&f, &i, sizeof i, 1);
    KH_CHECK_EQ(fortuna_generate(&f, out, sizeof out, 0), -1);
    KH_CHECK_EQ(f.seeded, 0u);
    int last = 255;
    fortuna_add(&f, &last, sizeof last, 1);
    KH_CHECK_EQ(f.seeded, 1u);
    KH_CHECK_EQ(fortuna_generate(&f, out, sizeof out, 0), 0);

    /* Uncredited input never seeds, however much of it. */
    fortuna_init(&f);
    for (int i = 0; i < 10000; i++) fortuna_add(&f, &i, sizeof i, 0);
    KH_CHECK_EQ(f.seeded, 0u);
}

KH_TEST(fortuna_erases_each_key_before_returning_bytes) {
    struct fortuna f;
    seed(&f, FORTUNA_SEED_BITS);
    uint8_t k0[32], blk[64], nonce[12] = { 0 }, a[100], b[100];
    memcpy(k0, f.key, 32);

    KH_CHECK_EQ(fortuna_generate(&f, a, sizeof a, 0), 0);
    /* The request's first block: its first 32 bytes are the new key, its
       last 32 the first bytes out; the next block follows on. */
    chacha20_block(k0, 0, nonce, blk);
    KH_CHECK(memcmp(f.key, blk, 32) == 0);
    KH_CHECK(memcmp(a, blk + 32, 32) == 0);
    chacha20_block(k0, 1, nonce, blk);
    KH_CHECK(memcmp(a + 32, blk, 64) == 0);
    chacha20_block(k0, 2, nonce, blk);
    KH_CHECK(memcmp(a + 96, blk, 4) == 0);

    KH_CHECK_EQ(fortuna_generate(&f, b, sizeof b, 0), 0);
    KH_CHECK(memcmp(a, b, sizeof a) != 0);
    KH_CHECK(memcmp(f.key, k0, 32) != 0);

    /* The same seed gives the same bytes: no hidden input. */
    struct fortuna g;
    seed(&g, FORTUNA_SEED_BITS);
    KH_CHECK_EQ(fortuna_generate(&g, b, sizeof b, 0), 0);
    KH_CHECK(memcmp(a, b, sizeof a) == 0);
}

KH_TEST(fortuna_reseeds_on_pool_0_and_the_interval) {
    struct fortuna f, g;
    seed(&f, FORTUNA_SEED_BITS);
    /* 8 bytes an event round 32 pools: 8 rounds give pool 0 its 64. */
    for (uint32_t i = 0; i < 8 * FORTUNA_POOLS; i++) {
        uint64_t ev = 0x1000 + i;
        fortuna_add(&f, &ev, sizeof ev, 7);
    }
    KH_CHECK_EQ(f.pool0_bytes, 64u);
    KH_CHECK_EQ(f.credit, (uint32_t)FORTUNA_SEED_BITS);   /* pools are never credited */
    memcpy(&g, &f, sizeof f);

    uint8_t a[32], b[32];
    KH_CHECK_EQ(fortuna_generate(&f, a, sizeof a, FORTUNA_RESEED_NS - 1), 0);
    KH_CHECK_EQ(f.reseeds, 0ull);
    KH_CHECK_EQ(fortuna_generate(&g, b, sizeof b, FORTUNA_RESEED_NS), 0);
    KH_CHECK_EQ(g.reseeds, 1ull);
    KH_CHECK_EQ(g.pool0_bytes, 0u);
    /* Reseed 1 drains pool 0 alone; pool 1 waits for reseed 2. */
    KH_CHECK_EQ(g.pool[0].bitlen, 0ull);
    KH_CHECK_EQ(g.pool[1].bitlen, 64ull * 8);
    KH_CHECK_EQ(g.pool[31].bitlen, 64ull * 8);
    KH_CHECK(memcmp(a, b, sizeof a) != 0);

    /* 63 bytes in pool 0 is not enough, whatever the time. */
    seed(&f, FORTUNA_SEED_BITS);
    for (uint32_t i = 0; i < 8 * FORTUNA_POOLS; i++) {
        uint64_t ev = i;                          /* pool 0's last is a byte short */
        fortuna_add(&f, &ev, i == 7 * FORTUNA_POOLS ? 7 : 8, 0);
    }
    KH_CHECK_EQ(f.pool0_bytes, 63u);
    KH_CHECK_EQ(fortuna_generate(&f, a, sizeof a, 10 * FORTUNA_RESEED_NS), 0);
    KH_CHECK_EQ(f.reseeds, 0ull);
}

KH_TEST(jitter_credits_nothing_to_a_counter_in_fixed_or_tiny_steps) {
    struct jitter_est e;
    uint32_t got = 0, t = 1000;

    memset(&e, 0, sizeof e);
    for (int i = 0; i < 100; i++) got += jitter_credit(&e, t += 1000);
    KH_CHECK_EQ(got, 0u);                         /* fixed steps */

    memset(&e, 0, sizeof e);
    got = 0;
    for (int i = 0; i < 100; i++) got += jitter_credit(&e, t += 1000 + (uint32_t)(i * 7 % 15));
    KH_CHECK_EQ(got, 0u);                         /* steps that wander by < 16 */

    memset(&e, 0, sizeof e);
    got = 0;
    for (int i = 0; i < 100; i++) got += jitter_credit(&e, t);
    KH_CHECK_EQ(got, 0u);                         /* stuck */

    /* Steps that wander widely: a bit each, but never for the first three. */
    static const uint32_t steps[] = { 1000, 1300, 900, 1700, 800, 2000, 650, 1900 };
    memset(&e, 0, sizeof e);
    got = 0;
    for (int i = 0; i < 3; i++) got += jitter_credit(&e, t += steps[i]);
    KH_CHECK_EQ(got, 0u);
    for (int i = 3; i < 8; i++) got += jitter_credit(&e, t += steps[i]);
    KH_CHECK_EQ(got, 5u);

    /* Each difference on its own: a step that alternates gives second
       differences of 0 between wide third ones (only the 500s earn), and
       a tiny step after a wide swing has wide second and third differences
       but a first of 5 (only the 300 earns). */
    static const uint32_t pairs[] = { 1000, 1000, 1500, 1500, 2000, 2000, 2500, 2500 };
    memset(&e, 0, sizeof e);
    got = 0;
    for (int i = 0; i < 8; i++) got += jitter_credit(&e, t += pairs[i]);
    KH_CHECK_EQ(got, 2u);
    static const uint32_t swing[] = { 100, 100, 100, 100, 300, 5, 300, 5 };
    memset(&e, 0, sizeof e);
    got = 0;
    for (int i = 0; i < 8; i++) got += jitter_credit(&e, t += swing[i]);
    KH_CHECK_EQ(got, 1u);

    /* Across the wrap of the 32-bit counter. */
    memset(&e, 0, sizeof e);
    got = 0;
    t = 0xFFFFF000u;
    for (int i = 0; i < 8; i++) got += jitter_credit(&e, t += steps[i]);
    KH_CHECK_EQ(got, 5u);
}

KH_TEST(rng_words_all_ones_zero_or_repeated_are_not_credited) {
    KH_CHECK(rng_word_plausible(0x0123456789ABCDEFull, 0));
    KH_CHECK(!rng_word_plausible(0, 5));
    KH_CHECK(!rng_word_plausible(~0ull, 5));
    KH_CHECK(!rng_word_plausible(0x42, 0x42));
}
