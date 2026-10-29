// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_clockscale.c */
/* Aurora Tejeda / CATX Systems */
/* lib/time/clockscale.h: counts at one rate to another, by multiply and shift. */
#include "khost.h"
#include "lib/time/clockscale.h"

#define NS 1000000000ull

/* 1 GHz is a nanosecond a cycle: exact, at every size. */
KH_TEST(clockscale_one_ghz_is_exact) {
    uint64_t f = clock_scale_factor(NS, NS);
    KH_CHECK(f == 1ull << 32);
    KH_CHECK(clock_scale(0, f) == 0);
    KH_CHECK(clock_scale(1, f) == 1);
    KH_CHECK(clock_scale(123456789ull, f) == 123456789ull);
    KH_CHECK(clock_scale(1ull << 62, f) == 1ull << 62);
}

/* The rig's 4.0 GHz FX-8350: a second of cycles is a second, a cycle rounds
   down to 0 ns, four are one. */
KH_TEST(clockscale_tsc_to_ns) {
    uint64_t f = clock_scale_factor(4000000000ull, NS);
    KH_CHECK(clock_scale(4000000000ull, f) == NS);
    KH_CHECK(clock_scale(1, f) == 0);
    KH_CHECK(clock_scale(4, f) == 1);
    /* an odd rate: within a nanosecond a second */
    f = clock_scale_factor(3999987654ull, NS);
    uint64_t s = clock_scale(3999987654ull, f);
    KH_CHECK(s <= NS && s >= NS - 1);
}

/* A year of a 4 GHz TSC would overflow 64 bits in the product without the
   128-bit multiply (1.26e17 cycles times a 2^30 factor). */
KH_TEST(clockscale_no_overflow_over_years) {
    uint64_t f = clock_scale_factor(4000000000ull, NS);
    uint64_t year = 365ull * 24 * 3600;
    uint64_t s = clock_scale(year * 4000000000ull, f);
    KH_CHECK(s <= year * NS && s >= year * NS - year);
}

/* Nanoseconds to a 62.5 MHz timer (QEMU's APIC clock over 16: a factor of
   2^28, exact), and to a 12.5 MHz one (a 200 MHz bus over 16, whose factor
   is rounded down): never more steps than the exact count, and at most one
   fewer, which is why timer.c's arm adds one. */
KH_TEST(clockscale_ns_to_timer_steps) {
    uint64_t k = clock_scale_factor(NS, 62500000ull);
    KH_CHECK(k == 1ull << 28);
    KH_CHECK(clock_scale(16, k) == 1);
    KH_CHECK(clock_scale(1000000ull, k) == 62500);
    k = clock_scale_factor(NS, 12500000ull);
    KH_CHECK(clock_scale(79, k) == 0);
    for (uint64_t ns = 80; ns <= NS; ns = ns * 3 + 7) {
        uint64_t exact = ns / 80, got = clock_scale(ns, k);
        KH_CHECK(got <= exact && got + 1 >= exact);
    }
    KH_CHECK(clock_scale(NS, k) + 1 >= 12500000ull);
}

/* What the factor refuses: a rate of 0, and a target of 2^32 or more, whose
   shift would lose its top bits. */
KH_TEST(clockscale_factor_refusals) {
    KH_CHECK(clock_scale_factor(0, NS) == 0);
    KH_CHECK(clock_scale_factor(NS, 1ull << 32) == 0);
    KH_CHECK(clock_scale_factor(NS, (1ull << 32) - 1) != 0);
}
