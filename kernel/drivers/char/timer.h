/* /CXLite/kernel/drivers/timer.h */
/* Aurora Tejeda */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Set up the PIT to fire IRQ0 at TIMER_HZ and install the handler. */
void timer_init(void);

/* Milliseconds since timer_init (at 1000 Hz, 1 tick = 1 ms). */
uint32_t timer_ticks(void);

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

#endif