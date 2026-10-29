// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/ktimer.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Timer objects (KOBJ_TIMER; docs/kernel/CX_KERNEL_PLAN.md section 5: "timers
 * are handles with absolute deadlines on the monotonic clock").
 *
 * A timer is armed for one deadline on timer_now_ns and fires once when the
 * tick reaches it: SIGNAL_TIMER_FIRED, which anything waiting on it sees as it
 * sees any other signal. Arming it again replaces the deadline and clears the
 * signal; cancelling disarms and clears. Nothing repeats by itself: a
 * periodic waiter re-arms for the next deadline it wants, which keeps the
 * period its own and drift out of the kernel.
 *
 * A timer's slack (fixed when it is made) lets it fire up to that much after
 * its deadline: the event timer is set for the earliest latest time among
 * them, and whatever interrupt comes fires every timer whose deadline it has
 * reached, so timers near each other share one (Linux's hrtimer soft and
 * hard expiry; Windows' tolerable delay). Slack 0 fires at the deadline.
 *
 * The armed timers are one list in deadline order, so the tick looks only at
 * its head. The list holds no reference: a timer leaves it when it fires,
 * when it is cancelled or re-armed, and when it is destroyed (with its last
 * reference: a wait on it outlives the last handle). One lock covers the
 * list; it is taken in the tick, and before the wait lock (kwait.c).
 */
#ifndef CXK_KTIMER_H
#define CXK_KTIMER_H

#include <stdint.h>
#include "object/kobj.h"

struct ktimer {
    struct kobj    hdr;        /* first; KOBJ_TIMER */
    uint64_t       deadline;   /* while armed */
    uint64_t       slack;      /* ns it may fire late by */
    struct ktimer *next;       /* in the armed list, in deadline order */
    uint8_t        armed;
};

void ktimer_sys_init(void);

/* A new timer, disarmed, that may fire up to `slack` ns after a deadline,
   with one reference for the caller (who installs a handle and drops it). 0
   when out of memory. */
struct ktimer *ktimer_create(uint64_t slack);

/* Arm `t` for `deadline` (nanoseconds on timer_now_ns) in place of any it had,
   clearing SIGNAL_TIMER_FIRED; a deadline already passed fires it now. */
void ktimer_set(struct ktimer *t, uint64_t deadline);

/* Disarm `t` and clear SIGNAL_TIMER_FIRED. */
void ktimer_cancel(struct ktimer *t);

/* From the tick, interrupts off: fire every armed timer whose deadline is at
   or before `now`. */
void ktimer_tick(uint64_t now);

/* The earliest latest time of an armed timer (deadline plus slack), or
   TIMER_NEVER: what the event timer is set for on their account. */
uint64_t ktimer_next_due(void);

/* Timers armed now, for the self-tests. */
uint32_t ktimer_armed_count(void);

#endif
