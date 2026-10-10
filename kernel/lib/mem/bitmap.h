// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/bitmap.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Pure bit-array operations over a caller-owned uint32_t[] backing store.
 * No hardware, no global state - just bit twiddling on memory you hand it, so
 * it lives in lib/ and is testable in isolation. pmm uses it for its physical
 * frame bitmap; a future block/inode bitmap (filesystem) or buddy allocator can
 * reuse the same code.
 *
 * Convention: bit set (1) = "used/allocated", clear (0) = "free", but the lib
 * is policy-agnostic - it just sets/clears/tests/searches bits. The caller
 * decides what 1 and 0 mean.
 */

#ifndef BITMAP_H
#define BITMAP_H

#include <stdint.h>
#include <stddef.h>

/* set / clear / test a single bit in words[] */
static inline void bitmap_set(uint32_t *words, uint32_t i)   { words[i >> 5] |=  (1u << (i & 31)); }
static inline void bitmap_clear(uint32_t *words, uint32_t i) { words[i >> 5] &= ~(1u << (i & 31)); }
static inline int  bitmap_test(uint32_t *words, uint32_t i)  { return (words[i >> 5] >> (i & 31)) & 1u; }

/* fill the first `nbits` bits with all-set (used) or all-clear (free). Operates
   word-at-a-time for speed; trailing bits beyond a word boundary are fine to
   over-set since callers track the real count separately. */
void bitmap_fill(uint32_t *words, uint32_t nbits, int set);

/* find the first CLEAR (free) bit in [0, nbits). Returns the index, or
   BITMAP_NONE if all bits in range are set. */
#define BITMAP_NONE 0xFFFFFFFFu
uint32_t bitmap_first_clear(uint32_t *words, uint32_t nbits);

/* find `count` CONSECUTIVE clear bits in [0, nbits). Returns the index of the
   first bit of the run, or BITMAP_NONE if no such run exists. */
uint32_t bitmap_first_clear_run(uint32_t *words, uint32_t nbits, uint32_t count);

#endif