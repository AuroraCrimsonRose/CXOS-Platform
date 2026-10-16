// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_bitmap.c */
/* Aurora Tejeda / CATX Systems */
/*
 * lib/mem/bitmap.c: the bit array under the physical page allocator.
 *
 * The searches are checked against a model that cannot be wrong in the same
 * way: a plain loop over bitmap_test. Random bitmaps of every awkward length
 * (word-aligned, one under, one over, shorter than a word) exercise the tail
 * handling, which is where a whole-word scan goes wrong.
 */
#include "khost.h"
#include "lib/mem/bitmap.h"

#include <string.h>

#define MAXBITS 300u

static uint32_t model_first_clear_run(uint32_t *w, uint32_t nbits, uint32_t count) {
    if (count == 0) return BITMAP_NONE;
    for (uint32_t s = 0; s + count <= nbits; s++) {
        uint32_t k = 0;
        while (k < count && !bitmap_test(w, s + k)) k++;
        if (k == count) return s;
    }
    return BITMAP_NONE;
}

KH_TEST(bitmap_set_clear_test_touch_only_their_bit) {
    uint32_t w[4];
    memset(w, 0, sizeof w);
    bitmap_set(w, 0);
    bitmap_set(w, 31);
    bitmap_set(w, 32);
    bitmap_set(w, 127);
    KH_CHECK_EQ(w[0], 0x80000001u);
    KH_CHECK_EQ(w[1], 0x00000001u);
    KH_CHECK_EQ(w[2], 0u);
    KH_CHECK_EQ(w[3], 0x80000000u);
    bitmap_clear(w, 31);
    KH_CHECK_EQ(w[0], 0x00000001u);
    KH_CHECK(bitmap_test(w, 0) && !bitmap_test(w, 1) && bitmap_test(w, 127));
}

KH_TEST(bitmap_first_clear_finds_the_tail_bit_and_stops_at_nbits) {
    uint32_t w[2];
    bitmap_fill(w, 64, 1);
    KH_CHECK_EQ(bitmap_first_clear(w, 64), BITMAP_NONE);

    bitmap_clear(w, 40);                       /* in the tail of a 41-bit map */
    KH_CHECK_EQ(bitmap_first_clear(w, 41), 40u);
    KH_CHECK_EQ(bitmap_first_clear(w, 40), BITMAP_NONE);   /* just outside */
}

KH_TEST(bitmap_first_clear_run_refuses_zero_and_runs_past_the_end) {
    uint32_t w[1] = { 0 };
    KH_CHECK_EQ(bitmap_first_clear_run(w, 32, 0), BITMAP_NONE);
    KH_CHECK_EQ(bitmap_first_clear_run(w, 32, 32), 0u);
    KH_CHECK_EQ(bitmap_first_clear_run(w, 32, 33), BITMAP_NONE);
    KH_CHECK_EQ(bitmap_first_clear_run(w, 10, 11), BITMAP_NONE);
}

KH_TEST(bitmap_searches_agree_with_a_model_on_random_maps) {
    uint64_t seed = 0xC0FFEEull;
    static const uint32_t lengths[] = { 1, 5, 31, 32, 33, 63, 64, 65, 200, MAXBITS };

    for (unsigned li = 0; li < sizeof lengths / sizeof lengths[0]; li++) {
        uint32_t nbits = lengths[li];
        for (int round = 0; round < 200; round++) {
            uint32_t w[(MAXBITS + 31) / 32 + 1];
            memset(w, 0, sizeof w);
            /* Mostly-set maps, so free runs are short and the search works. */
            for (uint32_t i = 0; i < nbits; i++)
                if (kh_rand(&seed) % 100 < 80) bitmap_set(w, i);
            /* Bits beyond nbits are set, as bitmap_fill leaves them, so a
               search that looks past the end finds nothing there either. */
            for (uint32_t i = nbits; i < (uint32_t)(sizeof w * 8); i++) bitmap_set(w, i);

            uint32_t want1 = model_first_clear_run(w, nbits, 1);
            KH_CHECK_EQ(bitmap_first_clear(w, nbits), want1);

            uint32_t count = 1 + kh_rand(&seed) % 6;
            KH_CHECK_EQ(bitmap_first_clear_run(w, nbits, count),
                        model_first_clear_run(w, nbits, count));
        }
    }
}
