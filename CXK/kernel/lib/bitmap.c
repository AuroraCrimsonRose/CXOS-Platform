/* /CXK/kernel/lib/bitmap.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Pure bit-array operations (see bitmap.h). */

#include "bitmap.h"

void bitmap_fill(uint32_t *words, uint32_t nbits, int set) {
    uint32_t nwords = (nbits + 31) / 32;
    uint32_t v = set ? 0xFFFFFFFFu : 0x00000000u;
    for (uint32_t i = 0; i < nwords; i++) words[i] = v;
}

uint32_t bitmap_first_clear(uint32_t *words, uint32_t nbits) {
    uint32_t nwords = nbits / 32;
    /* scan whole words first - a fully-set word (0xFFFFFFFF) has no free bit */
    for (uint32_t w = 0; w < nwords; w++) {
        if (words[w] != 0xFFFFFFFFu) {
            for (uint32_t b = 0; b < 32; b++)
                if (!((words[w] >> b) & 1u)) return w * 32 + b;
        }
    }
    /* check any leftover tail bits (nbits not a multiple of 32) */
    for (uint32_t i = nwords * 32; i < nbits; i++)
        if (!bitmap_test(words, i)) return i;
    return BITMAP_NONE;
}

uint32_t bitmap_first_clear_run(uint32_t *words, uint32_t nbits, uint32_t count) {
    if (count == 0) return BITMAP_NONE;
    if (count == 1) return bitmap_first_clear(words, nbits);

    uint32_t run = 0, start = 0;
    for (uint32_t i = 0; i < nbits; i++) {
        if (!bitmap_test(words, i)) {
            if (run == 0) start = i;
            if (++run == count) return start;
        } else {
            run = 0;
        }
    }
    return BITMAP_NONE;
}