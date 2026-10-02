/* /kernel/lib/align.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Pure alignment helpers. No hardware, no state - just integer math, so this
 * lives in lib/. pmm/heap/loaders call these instead of open-coding the
 * round-up/round-down dance (the classic `(x + a - 1) & ~(a - 1)` that's easy
 * to get subtly wrong). `a` (the alignment) must be a power of two.
 *
 * static inline (header-only): type-safe, no call overhead, no macro
 * double-evaluation surprises.
 */

#ifndef ALIGN_H
#define ALIGN_H

#include <stdint.h>

/* round x UP to the next multiple of a (a must be a power of two) */
static inline uint32_t align_up(uint32_t x, uint32_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

/* round x DOWN to the previous multiple of a (a must be a power of two) */
static inline uint32_t align_down(uint32_t x, uint32_t a) {
    return x & ~(a - 1);
}

/* nonzero if x is already a multiple of a (a must be a power of two) */
static inline int is_aligned(uint32_t x, uint32_t a) {
    return (x & (a - 1)) == 0;
}

#endif