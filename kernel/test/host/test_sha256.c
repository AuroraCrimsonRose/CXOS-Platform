// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_sha256.c */
/* Aurora Tejeda / CATX Systems */
/*
 * lib/crypto/sha256.c: the hash every signature check rests on.
 *
 * The digests are the published FIPS 180-4 examples (NIST CSRC "SHA-256
 * example values"), not values this code produced, so a mistake cannot agree
 * with itself. The streaming test feeds the same message in every split, which
 * is where block-boundary bugs live.
 */
#include "khost.h"
#include "lib/crypto/sha256.h"

#include <stdlib.h>
#include <string.h>

static int digest_is(const uint8_t d[32], const char *hex) {
    for (int i = 0; i < 32; i++) {
        unsigned v;
        char pair[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        v = (unsigned)strtoul(pair, NULL, 16);
        if (d[i] != (uint8_t)v) return 0;
    }
    return 1;
}

KH_TEST(sha256_matches_the_fips_180_4_examples) {
    uint8_t d[32];

    sha256("", 0, d);
    KH_CHECK(digest_is(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    sha256("abc", 3, d);
    KH_CHECK(digest_is(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two_blocks, strlen(two_blocks), d);
    KH_CHECK(digest_is(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

KH_TEST(sha256_of_a_million_a_streamed_in_odd_pieces) {
    static uint8_t chunk[997];
    memset(chunk, 'a', sizeof chunk);

    struct sha256_ctx c;
    sha256_init(&c);
    size_t left = 1000000;
    while (left) {
        size_t n = left < sizeof chunk ? left : sizeof chunk;
        sha256_update(&c, chunk, n);
        left -= n;
    }
    uint8_t d[32];
    sha256_final(&c, d);
    KH_CHECK(digest_is(d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

KH_TEST(sha256_streaming_agrees_with_one_shot_at_every_split) {
    uint8_t msg[200];
    for (unsigned i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i * 7 + 3);

    /* Lengths around the 55/56/64-byte padding boundaries, and every split. */
    static const size_t lens[] = { 0, 1, 55, 56, 63, 64, 65, 119, 120, 128, 200 };
    for (unsigned li = 0; li < sizeof lens / sizeof lens[0]; li++) {
        size_t len = lens[li];
        uint8_t want[32];
        sha256(msg, len, want);
        for (size_t split = 0; split <= len; split++) {
            struct sha256_ctx c;
            uint8_t got[32];
            sha256_init(&c);
            sha256_update(&c, msg, split);
            sha256_update(&c, msg + split, len - split);
            sha256_final(&c, got);
            KH_CHECK(memcmp(got, want, 32) == 0);
        }
    }
}
