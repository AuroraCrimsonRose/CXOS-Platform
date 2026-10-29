// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/time/clockscale.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Converting between two counters' rates without dividing on the way: a
 * count of one becomes the other's by a multiply and a shift, the way Linux
 * turns TSC cycles into nanoseconds (cyc2ns). The factor is the second rate
 * over the first in 32.32 fixed point, worked out once when the rates are
 * measured; the product is taken in 128 bits, so any 64-bit count converts
 * without overflowing on the way. drivers/char/timer.c uses it for TSC
 * cycles to nanoseconds and nanoseconds to local APIC timer steps.
 *
 * The one division (clock_scale_factor) stays in 64 bits, since a
 * freestanding kernel has no 128-bit divide (__udivti3): `to_hz` must be
 * below 2^32, which both uses meet (a nanosecond's 10^9; a timer's rate).
 */

#ifndef CXK_CLOCKSCALE_H
#define CXK_CLOCKSCALE_H

#include <stdint.h>

/* The factor that turns counts at `from_hz` into counts at `to_hz`; 0 for a
   rate of 0 or a `to_hz` too large to fit. */
static inline uint64_t clock_scale_factor(uint64_t from_hz, uint64_t to_hz) {
    if (from_hz == 0 || to_hz >= (1ull << 32)) return 0;
    return (to_hz << 32) / from_hz;
}

/* `count` at the first rate, at the second, rounded down; the top 64 bits
   are dropped (a count that large is centuries of any clock). */
static inline uint64_t clock_scale(uint64_t count, uint64_t factor) {
    return (uint64_t)(((unsigned __int128)count * factor) >> 32);
}

#endif
