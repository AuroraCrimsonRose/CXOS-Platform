// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/char/timer.c */
/* Aurora Tejeda */
/* The clock and the tick: see timer.h. */

#include <stdint.h>
#include "sched.h"
#include "timer.h"
#include "proc.h"         /* proc_stop_tick */
#include "object/ktimer.h"
#include "arch/irq.h"
#include "arch/cpu_ops.h"
#include "arch/evtimer.h"
#include "lib/time/clockscale.h"
#include "platform/platform.h"   /* the PIT's tick */

#define TIMER_HZ      1000u            /* 1000 Hz -> ~1 ms per tick */
#define NS_PER_SEC    1000000000ull

/* The longest the event timer is left set for. Every deadline asks for its
   own interrupt (timer_deadline_hint), so this is never what meets one; it
   bounds what a deadline that forgot to ask would cost: a second, not
   forever. Linux's NO_HZ has no such bound, and a lost wakeup there is a hang
   until something else interrupts. */
#define EV_MAX_NS     NS_PER_SEC
/* The shortest: a deadline closer than this, or passed, is set this far out
   rather than as a count of 0, which stops the timer instead. */
#define EV_MIN_NS     2000ull

/* ---- the clock ----
 * The PIT's ticks until timer_calibrate_tsc measures the TSC, and the TSC
 * from then on, carried on from the tick clock's last value so it never goes
 * back. ticks64 keeps counting while the PIT interrupts, but once the TSC is
 * the clock nothing reads it as one. */
static volatile uint64_t ticks64 = 0;
static int      clock_tsc = 0;          /* timer_now_ns reads the TSC */
static uint64_t tsc_hz = 0;
static uint64_t tsc_base, ns_base;      /* the TSC clock read ns_base at tsc_base */
static uint64_t ns_per_cycle;           /* clock_scale factor */
static uint32_t tsc_per_us = 0;         /* cycles per microsecond; 0 = not calibrated */
static uint32_t tick_offset = 0;        /* timer_ticks_set_for_test */

/* ---- the event source ----
 * What interrupts to run the expiries: the PIT every millisecond, or the
 * local APIC's timer set once for the earliest thing due. */
static volatile int ev_source = TIMER_SOURCE_PIT;
static uint64_t ev_due = TIMER_NEVER;   /* what the APIC timer is set for */
static uint64_t steps_per_ns;           /* clock_scale factor, ns to timer steps */
static uint64_t ev_hz = 0;
static volatile uint64_t ev_count = 0;  /* expiry runs: timer_events */
static uint64_t halt_due = TIMER_NEVER; /* timer_halt_until's */
static int      idling = 0;             /* in timer_idle: no time slice to end */

uint64_t timer_now_ns(void) {
    if (!clock_tsc) return ticks64 * TIMER_NS_PER_TICK;
    return ns_base + clock_scale(arch_cycles64() - tsc_base, ns_per_cycle);
}

uint32_t timer_ticks(void) {
    return (uint32_t)(timer_now_ns() / TIMER_NS_PER_TICK) + tick_offset;
}

/* Everything due by `now`, interrupts off. Each looks at the clock it is
   given, not the tick it came from: under the APIC's timer there is none. */
static void run_expiries(uint64_t now) {
    ev_count++;
    if (halt_due <= now) halt_due = TIMER_NEVER;
    sched_tick(now);          /* the time slice (no-op unless preemption is on) */
    sched_wake_sleepers(now); /* SYS_SLEEP, a wait's deadline */
    ktimer_tick(now);         /* timer objects */
    proc_stop_tick(now);      /* SYS_PROCESS_STOP's deadlines */
}

/* The earliest anything needs the CPU back, interrupts off. A timer object
   with slack gives its latest time, not its deadline (ktimer.h). */
static uint64_t next_due(void) {
    uint64_t d = halt_due, x;
    if ((x = sched_next_due(!idling)) < d) d = x;
    if ((x = ktimer_next_due()) < d) d = x;
    if ((x = proc_stop_next_due()) < d) d = x;
    return d;
}

/* Set the APIC's timer for `due`, interrupts off. Rounded up a step, so it
   never fires before `due`; one that comes early all the same (the EV_MAX_NS
   bound) finds nothing due and sets it again. */
static void arm(uint64_t due) {
    uint64_t now = timer_now_ns();
    if (due > now + EV_MAX_NS) due = now + EV_MAX_NS;
    uint64_t dt = due > now ? due - now : 0;
    if (dt < EV_MIN_NS) dt = EV_MIN_NS;
    uint64_t steps = clock_scale(dt, steps_per_ns) + 1;
    if (steps > 0xFFFFFFFFull) steps = 0xFFFFFFFFull;
    ev_due = due;
    arch_evtimer_start((uint32_t)steps);
}

static void program(void) {
    if (ev_source == TIMER_SOURCE_APIC) arm(next_due());
}

static void pit_tick(struct registers *r) {
    (void)r;
    ticks64++;
    if (ev_source == TIMER_SOURCE_PIT) run_expiries(timer_now_ns());
}

static void apic_fire(struct registers *r) {
    (void)r;
    if (ev_source != TIMER_SOURCE_APIC) return;   /* the calibration's count */
    ev_due = TIMER_NEVER;
    run_expiries(timer_now_ns());
    program();
}

void timer_deadline_hint(uint64_t due) {
    uint32_t f = arch_irq_save();
    if (ev_source == TIMER_SOURCE_APIC && due < ev_due) arm(due);
    arch_irq_restore(f);
}

void timer_idle(void) {
    idling = 1;
    program();
    arch_cpu_idle_irqs_off();
    idling = 0;
    sched_slice_restart(timer_now_ns());
    program();
}

void timer_halt_until(uint64_t due) {
    if (!arch_irqs_enabled()) return;   /* nothing would wake it */
    uint32_t f = arch_irq_save();
    if (!sched_others_ready()) {
        if (due < halt_due) halt_due = due;
        timer_idle();
    }
    arch_irq_restore(f);
}

void timer_sleep(uint32_t ms) {
    /* If interrupts are disabled, the clock may not advance (the tick
       interrupt can't fire), so the normal wait would hang forever. Detect
       that and fall back to a bounded busy-delay instead - prevents a
       deadlock if timer_sleep is ever called during early boot init (before
       sti) or inside a cli section. */
    if (!arch_irqs_enabled()) {
        volatile uint32_t x = 0;
        /* rough busy-delay: ~ a few hundred thousand iterations per ms */
        for (uint32_t i = 0; i < ms * 200000u; i++) x += i;
        return;
    }
    /* On the 64-bit clock, which does not wrap: timer_ticks_set_for_test
       moves the 32-bit tick, not this. */
    uint64_t due = timer_now_ns() + (uint64_t)ms * TIMER_NS_PER_TICK;
    while (timer_now_ns() < due) timer_halt_until(due);
}

/* ---- bounded-wait helpers (see timer.h) ---- */

/* spin budget per millisecond when interrupts are off (rough, deliberately
   generous so a slow device isn't cut off early on real hardware). */
#define TIMEOUT_SPINS_PER_MS  200000u

void timer_timeout_start(struct timeout *to, uint32_t ms) {
    if (arch_irqs_enabled()) {           /* interrupts on: use ticks */
        to->use_timer  = 1;
        to->deadline   = timer_ticks() + ms + 1; /* +1 so sub-ms budgets get a full tick */
        to->spins_left = 0;
    } else {                             /* interrupts off: bounded spin */
        to->use_timer  = 0;
        to->deadline   = 0;
        to->spins_left = ms * TIMEOUT_SPINS_PER_MS;
        if (to->spins_left == 0) to->spins_left = TIMEOUT_SPINS_PER_MS;
    }
}

int timer_timeout_expired(struct timeout *to) {
    if (to->use_timer) {
        /* Wrap-safe: a plain `ticks >= to->deadline` reports expiry the
           instant the deadline sum wraps, which would fail a disk transfer
           with DISK_ERR_TIMEOUT before the hardware was even asked. */
        return timer_tick_after(timer_ticks(), to->deadline) ? 1 : 0;
    }
    /* spin mode: each call burns one unit of budget */
    if (to->spins_left == 0) return 1;
    to->spins_left--;
    return 0;
}

void timer_init(void) {
    platform_tick_start(TIMER_HZ, pit_tick);
}

/* ---- the TSC ----
 * Measured against the tick: 50 of them, from a tick's edge. The PIT's
 * millisecond is its divisor's (1193 of 1193182 Hz, 0.015% short), and the
 * TSC clock inherits it, so the clock keeps the rate it had. */
void timer_calibrate_tsc(void) {
    if (!arch_cycles_constant() || !arch_irqs_enabled()) return;
    uint64_t t0 = ticks64;
    while (ticks64 == t0) { }                /* align to a tick edge */

    uint64_t c0    = arch_cycles64();
    uint64_t start = ticks64;
    while (ticks64 - start < 50) { }         /* 50 ms window */
    uint64_t cycles = arch_cycles64() - c0;

    uint64_t hz = cycles * (TIMER_HZ / 50);
    uint64_t f  = clock_scale_factor(hz, NS_PER_SEC);
    if (hz < 1000000ull || f == 0) return;   /* stays on the tick */
    tsc_hz = hz;
    tsc_per_us = (uint32_t)(hz / 1000000ull);

    uint32_t fl = arch_irq_save();
    ns_base      = ticks64 * TIMER_NS_PER_TICK;
    tsc_base     = arch_cycles64();
    ns_per_cycle = f;
    clock_tsc    = 1;
    arch_irq_restore(fl);
}

uint32_t timer_tsc32(void) { return arch_cycles32(); }

/* Microseconds elapsed since a timer_tsc32() sample. */
uint32_t timer_us_since(uint32_t start) {
    if (!tsc_per_us) return 0;
    return (arch_cycles32() - start) / tsc_per_us;
}

uint32_t timer_tsc_mhz(void) { return tsc_per_us; }
uint64_t timer_tsc_hz(void)  { return tsc_hz; }
int      timer_clock_is_tsc(void) { return clock_tsc; }

/* ---- the local APIC's timer ----
 * Measured against the TSC clock over 10 ms, from a count it cannot finish
 * (2^32 steps is seconds at any rate it could have). Then it is the tick's
 * source, and the PIT's interrupt is masked: the PIT keeps counting, which
 * the fallback test uses (timer_use_pit_for_test). */
int timer_start_deadlines(void) {
    if (!clock_tsc) return 0;   /* between its interrupts the clock must run */
    if (arch_evtimer_init(apic_fire) != 0) return 0;

    uint32_t f = arch_irq_save();
    arch_evtimer_start(0xFFFFFFFFu);
    uint64_t n0 = timer_now_ns();
    arch_irq_restore(f);
    while (timer_now_ns() - n0 < 10 * TIMER_NS_PER_TICK) arch_cpu_relax();
    f = arch_irq_save();
    uint32_t left = arch_evtimer_left();
    uint64_t n1 = timer_now_ns();
    arch_evtimer_start(0);
    arch_irq_restore(f);

    uint64_t hz = (uint64_t)(0xFFFFFFFFu - left) * NS_PER_SEC / (n1 - n0);
    uint64_t k  = clock_scale_factor(NS_PER_SEC, hz);
    if (left == 0 || hz < 100000ull || k == 0) return 0;   /* stays on the PIT */
    ev_hz = hz;
    steps_per_ns = k;

    f = arch_irq_save();
    platform_tick_stop();
    ev_source = TIMER_SOURCE_APIC;
    program();
    arch_irq_restore(f);
    return 1;
}

int      timer_source(void)    { return ev_source; }
uint64_t timer_pit_ticks(void) { return ticks64; }
uint64_t timer_events(void)    { return ev_count; }
uint64_t timer_evtimer_hz(void) { return ev_hz; }

/* ---- testing only: see timer.h ---- */
void timer_ticks_set_for_test(uint32_t t) {
    tick_offset += t - timer_ticks();
}

int timer_use_pit_for_test(int on) {
    if (!ev_hz) return 0;   /* never left it */
    uint32_t f = arch_irq_save();
    if (on) {
        ev_source = TIMER_SOURCE_PIT;
        arch_evtimer_start(0);
        ev_due = TIMER_NEVER;
        platform_tick_resume();
    } else {
        platform_tick_stop();
        ev_source = TIMER_SOURCE_APIC;
        program();
    }
    arch_irq_restore(f);
    return 1;
}
