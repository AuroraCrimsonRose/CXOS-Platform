// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/timer.h */
/* Aurora Tejeda */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Set up the PIT to fire IRQ0 at TIMER_HZ and install the handler. */
void timer_init(void);

/* Milliseconds since timer_init (at 1000 Hz, 1 tick = 1 ms). */
uint32_t timer_ticks(void);

/* ---- comparing tick values ----
 * The tick counter is 32 bits, so it wraps back to zero after 2^32 ms, about
 * 49.7 days of uptime. A deadline computed as `now + ms` therefore compares
 * *smaller* than the present once the sum has wrapped, and the two obvious
 * forms both fail there in opposite directions: `now < deadline` ends a wait
 * immediately, and `now >= deadline` fires a timeout immediately.
 *
 * Every comparison of two tick values goes through these instead. The
 * difference is taken first and read as signed, so only the distance between
 * the two matters and the wrap cancels out.
 *
 * The one condition: the values must lie within 2^31 ticks (~24.8 days) of
 * each other, which every wait in this kernel satisfies by a wide margin -
 * the longest is SLEEP_MAX_MS, one hour.
 *
 * This is the approach the counter's width forces. The alternative is a
 * 64-bit counter, which never wraps but cannot be read atomically on a 32-bit
 * machine: NT keeps `KeTickCount` as 64 bits and every i386 reader is a retry
 * loop over High1Time/LowPart/High2Time (`ntos/inc/i386.h`). A single-word
 * counter keeps timer_ticks() a plain load, which matters because it is read
 * from inside the tick handler itself.
 */

/* 1 if tick `a` is at or after tick `b`. */
static inline int timer_tick_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) >= 0;
}

/* 1 if tick `a` is strictly before tick `b`. */
static inline int timer_tick_before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

/* Milliseconds elapsed since `start`. Correct across the wrap for any
   interval shorter than the counter's whole period. */
static inline uint32_t timer_since(uint32_t start) {
    return timer_ticks() - start;
}

/* Busy-wait for the given number of milliseconds. */
void timer_sleep(uint32_t ms);

/* ---- bounded-wait helpers for driver hardware loops ----
 * These give a true wall-clock timeout when interrupts are on (using the PIT
 * tick counter), and fall back to a bounded spin when interrupts are OFF (e.g.
 * during boot init, before sti) - so they never deadlock waiting on a tick that
 * can't advance.
 *
 * Usage:
 *   struct timeout to;
 *   timeout_start(&to, 500);            // 500 ms budget
 *   while (HARDWARE_BUSY()) {
 *       if (timeout_expired(&to)) return ERR_TIMEOUT;
 *   }
 */
struct timeout {
    int      use_timer;   /* 1 = interrupts were on, use ticks */
    uint32_t deadline;    /* tick deadline (if use_timer) */
    uint32_t spins_left;  /* fallback spin budget (if !use_timer) */
};

/* begin a timeout of `ms` milliseconds. */
void timer_timeout_start(struct timeout *to, uint32_t ms);

/* returns 1 if the timeout has expired. call this in the wait loop. */
int  timer_timeout_expired(struct timeout *to);

/* ---- sub-millisecond timing (TSC) ----
   timer_ticks() has 1 ms granularity; these give microseconds. */
void     timer_calibrate_tsc(void);   /* call once after timer_init */
uint32_t timer_tsc32(void);           /* raw cycle counter (low 32 bits) */
uint32_t timer_us_since(uint32_t start_tsc);
uint32_t timer_tsc_mhz(void);         /* cycles per microsecond, 0 if uncalibrated */

/* ---- testing only ----
 * Moves the tick counter. This exists so ktest.c can drive the 32-bit wrap at
 * 2^32 without 49.7 days of uptime; nothing else may call it, because every
 * deadline already in flight is relative to the value it replaces. */
void timer_ticks_set_for_test(uint32_t t);

#endif