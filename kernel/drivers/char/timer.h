// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/timer.h */
/* Aurora Tejeda */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/*
 * ---- How time is kept (Phase 1.2c) ----
 *
 * A clock and an event source, chosen separately at boot:
 *
 *   the clock    the PIT's tick counted in milliseconds until
 *                timer_calibrate_tsc measures the TSC, then the TSC scaled
 *                to nanoseconds, carried on from the tick clock's value so it
 *                never goes back. A CPU with no TSC keeps the tick.
 *   the source   the PIT's 1000 Hz interrupt until timer_start_deadlines
 *                measures the local APIC's timer against the TSC clock; from
 *                then on that timer is set once for the earliest thing due
 *                and the PIT's IRQ 0 is masked. Without a TSC clock, a local
 *                APIC, or a measurement that makes sense, the PIT stays.
 *
 * Either way one function runs what is due (timer.c's run_expiries): the
 * time slice, sleepers and waits, timer objects, stop deadlines. Under the
 * APIC's timer each of those says when it next needs the CPU, the timer is
 * set for the earliest (a timer object with slack: its latest), and a
 * deadline made between interrupts asks for its own with
 * timer_deadline_hint. An idle CPU then takes an interrupt when something is
 * due and not every millisecond.
 */

/* Set up the PIT to fire IRQ0 at TIMER_HZ and install the handler. */
void timer_init(void);

/* Milliseconds since timer_init: the monotonic clock's, in 32 bits (plus
   what timer_ticks_set_for_test moved it by). It does not need an interrupt
   to advance once the clock is the TSC. */
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

/* ---- the monotonic clock (SYS_CLOCK_MONOTONIC) ----
 * Nanoseconds since timer_init, 64 bits: it never wraps (292 years to 2^63),
 * so a deadline on it is compared plainly. A millisecond at a time on the
 * tick; to the TSC's grain once that is the clock. Every deadline
 * (kobj_wait, sleeps, the timer objects, stops) is a time on this clock, and
 * TIMER_NEVER (DEADLINE_NEVER's value) is none. */
#define TIMER_NS_PER_TICK 1000000ull
#define TIMER_NEVER       0x7FFFFFFFFFFFFFFFull
uint64_t timer_now_ns(void);

/* Something is due at `due` on timer_now_ns: if the event timer is set for
   later, set it for this. A deadline made outside the timer's interrupt
   calls it once it is where next_due will find it (the sleeper marked, the
   timer object armed); any interrupts state. */
void timer_deadline_hint(uint64_t due);

/* Wait for an interrupt with nothing to run, interrupts off: the event timer
   is set for the next deadline alone (no time slice to end), and once an
   interrupt comes the slice starts again. Returns with interrupts off. The
   scheduler's idle loops; the caller checked there is nothing to run with
   interrupts off, since a wake that lands between that check and the halt
   would wait for the next deadline. */
void timer_idle(void);

/* Halt until an interrupt, or `due` at the latest, unless another thread is
   ready (then it returns at once). For loops that wait on the clock without
   blocking (kmain's idle loop, timer_sleep, the C1 idle); with interrupts off
   it returns at once. */
void timer_halt_until(uint64_t due);

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

/* ---- the TSC ----
   Measured once after timer_init, with interrupts on; from then on it is the
   clock, when it can be one (arch_cycles_constant). The 32-bit forms give
   microseconds across short intervals. */
void     timer_calibrate_tsc(void);   /* call once after timer_init */
uint32_t timer_tsc32(void);           /* raw cycle counter (low 32 bits) */
uint32_t timer_us_since(uint32_t start_tsc);
uint32_t timer_tsc_mhz(void);         /* cycles per microsecond, 0 if uncalibrated */
uint64_t timer_tsc_hz(void);          /* cycles per second, 0 if uncalibrated */
int      timer_clock_is_tsc(void);

/* ---- the event source ----
   timer_start_deadlines, once the local APIC is up (irq_controller_init):
   1 if its timer took over from the PIT. */
#define TIMER_SOURCE_PIT  0
#define TIMER_SOURCE_APIC 1
int      timer_start_deadlines(void);
int      timer_source(void);          /* TIMER_SOURCE_* */
uint64_t timer_events(void);          /* interrupts that ran the expiries */
uint64_t timer_evtimer_hz(void);      /* the APIC timer's steps a second, 0 unused */
uint64_t timer_pit_ticks(void);       /* PIT interrupts taken, either source */

/* ---- testing only ----
 * Moves the tick counter. This exists so ktest.c can drive the 32-bit wrap at
 * 2^32 without 49.7 days of uptime; nothing else may call it, because every
 * deadline already in flight is relative to the value it replaces. It moves
 * timer_ticks only: the 64-bit clock, and every deadline on it, stay. */
void timer_ticks_set_for_test(uint32_t t);

/* Back to the PIT as the source (1), or to the APIC's timer again (0), so
   the fallback is tested on a machine that does not need it. 0 when the
   APIC's timer never took over. */
int  timer_use_pit_for_test(int on);

#endif