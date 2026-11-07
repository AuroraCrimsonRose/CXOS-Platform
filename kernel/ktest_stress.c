// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_stress.c */
/* Aurora Tejeda / CATX Systems */
/* The SMP stress suite's own tests and its profile - see ktest_stress.h. */

#include <stdint.h>
#include "config.h"
#include "ktest_stress.h"
#include "sched.h"
#include "timer.h"
#include "logging.h"
#include "serial.h"
#include "console.h"            /* the log lock over a report line */
#include "cpu/percpu.h"
#include "arch/smp.h"
#include "arch/cpu_ops.h"
#include "diag/hwfmt.h"

#define MS 1000000ull

static int st_fail(const char *what, uint32_t step) {
    klog_u32("KTEST", SEV_FAIL, what, step, LOG_COLOR_VALUE, "");
    return 0;
}

#if CXK_TEST_HOOKS

static struct lockstat sched_stat_of(unsigned i) {
    return cpu_at(i)->lockstat[LOCKSTAT_SCHED];
}

/* ---- the count ----
 * The boot CPU holds sched_lock while AP 1 takes it: the AP's take must be
 * counted contended, with spins and the time it waited, and the boot CPU's
 * longest hold must be at least the hold it timed itself. */
static volatile uint32_t lc_ready, lc_go, lc_done;

static void lc_ap(void *arg) {
    (void)arg;
    uint64_t end = arch_cycles64() + 4000000000ull;   /* a second at 4 GHz; longer if slower */
    lc_ready = 1;
    while (!lc_go && arch_cycles64() < end) arch_cpu_relax();
    uint32_t f = sched_lock_test_take();
    sched_lock_test_give(f);
    lc_done = 1;
}

static int wait_flag(volatile uint32_t *flag, uint64_t ms) {
    uint64_t end = timer_now_ns() + ms * MS;
    while (!*flag && timer_now_ns() < end) arch_cpu_relax();
    return *flag != 0;
}

int ktest_lockstat_count(void) {
    if (cpu_count() < 2) return 1;
    lc_ready = lc_go = lc_done = 0;
    struct lockstat a0 = sched_stat_of(1);
    if (smp_call_start(1, lc_ap, 0) != 0) return st_fail("lockstat count: step ", 1);
    if (!wait_flag(&lc_ready, 1000)) {
        lc_go = 1;
        (void)smp_call_wait(1, 2000 * MS);
        return st_fail("lockstat count: step ", 2);
    }

    /* Held for 20 ms of the clock with the AP spinning on it. Interrupts
       are off: shootdowns are served by hand, as a spin would. */
    uint32_t f = sched_lock_test_take();
    uint64_t t_a = arch_cycles64();
    lc_go = 1;
    uint64_t end = timer_now_ns() + 20 * MS;
    while (timer_now_ns() < end) { smp_serve(); arch_cpu_relax(); }
    uint64_t t_b = arch_cycles64();
    sched_lock_test_give(f);

    if (smp_call_wait(1, 2000 * MS) != 0) return st_fail("lockstat count: step ", 3);
    if (!lc_done) return st_fail("lockstat count: step ", 4);
    struct lockstat a1 = sched_stat_of(1);
    if (a1.acquired  <= a0.acquired)  return st_fail("lockstat count: step ", 5);
    if (a1.contended <= a0.contended) return st_fail("lockstat count: step ", 6);
    if (a1.spins     <= a0.spins)     return st_fail("lockstat count: step ", 7);
    /* the AP spun from just after the go to the give: most of the hold */
    if (a1.wait_tsc - a0.wait_tsc < (t_b - t_a) / 2) return st_fail("lockstat count: step ", 8);
    if (sched_stat_of(0).max_hold < t_b - t_a) return st_fail("lockstat count: step ", 9);
    return 1;
}

/* ---- the stress ----
 * One thread on each CPU. A token goes round them, each blocking until it
 * holds it and waking the next, so every pass is a wake made on one CPU for
 * a thread on another (its IPI included); then each yields, all at once.
 * Every pass must be made, none of the waits may need its timeout (a wake
 * lost), and nothing may be left behind. */
#define ST_LAPS      200u
#define ST_YIELDS    2000u
#define ST_WAIT_MS   1000u      /* a wait's own deadline: never reached */
#define ST_NONE      0xFFFFFFFFu

static volatile uint32_t st_token, st_done, st_slow, st_passes, st_abort;
static volatile uint32_t st_cpus;
static volatile int      st_ids[CPU_MAX];

static struct lockstat st_before[CPU_MAX], st_after[CPU_MAX];
static uint64_t st_ticks;
static int      st_ran;

static void st_thread(void) {
    uint32_t me = cpu_this()->index;   /* pinned: one thread per CPU */
    int self = thread_current_id();
    uint32_t n = st_cpus;
    for (uint32_t lap = 0; lap < ST_LAPS; lap++) {
        while (__atomic_load_n(&st_token, __ATOMIC_ACQUIRE) != me) {
            if (st_abort) return;
            uint64_t t0 = timer_now_ns();
            uint32_t f = arch_irq_save();
            thread_block_prepare(ST_WAIT_MS);
            if (__atomic_load_n(&st_token, __ATOMIC_ACQUIRE) == me) thread_unblock(self);
            arch_irq_restore(f);
            thread_block_finish();
            if (timer_now_ns() - t0 >= (uint64_t)(ST_WAIT_MS - 100u) * MS)
                __atomic_add_fetch(&st_slow, 1u, __ATOMIC_RELAXED);
        }
        __atomic_add_fetch(&st_passes, 1u, __ATOMIC_RELAXED);
        uint32_t next = (me + 1) % n;
        __atomic_store_n(&st_token, next, __ATOMIC_RELEASE);
        thread_unblock(st_ids[next]);
    }
    for (uint32_t i = 0; i < ST_YIELDS; i++) yield();
    __atomic_add_fetch(&st_done, 1u, __ATOMIC_RELEASE);
}

static int st_reaped(uint32_t n0) {
    for (int i = 0; i < 5000; i++) {
        if (sched_thread_count() == n0) return 1;
        thread_sleep_ms(1);
    }
    return 0;
}

int ktest_smp_stress(void) {
    unsigned n = cpu_count();
    if (n < 2) return 1;
    uint32_t n0 = sched_thread_count(), lim0 = sched_thread_limit();
    if (n0 + n > lim0 && sched_configure(n0 + n, sched_stack_bytes()) != 0)
        return st_fail("smp stress: step ", 1);

    st_token = ST_NONE; st_done = st_slow = st_passes = st_abort = 0; st_cpus = n;
    for (unsigned i = 0; i < n; i++) st_before[i] = sched_stat_of(i);
    uint64_t t0 = arch_cycles64();
    int step = 0;
    unsigned made = 0;
    for (; made < n; made++) {
        int id = thread_create_on("stress", st_thread, 1u << made);
        if (id < 0) { step = 2; break; }
        st_ids[made] = id;
    }
    if (!step) {
        __atomic_store_n(&st_token, 0u, __ATOMIC_RELEASE);
        thread_unblock(st_ids[0]);
        uint64_t end = timer_now_ns() + 120000ull * MS;
        while (__atomic_load_n(&st_done, __ATOMIC_ACQUIRE) != n && timer_now_ns() < end)
            thread_sleep_ms(5);
        if (st_done != n) step = 3;
    } else {
        /* the ones made wait for a token that never comes: end them */
        st_abort = 1;
        for (unsigned i = 0; i < made; i++) thread_unblock(st_ids[i]);
    }
    st_ticks = arch_cycles64() - t0;
    for (unsigned i = 0; i < n; i++) st_after[i] = sched_stat_of(i);
    st_ran = 1;

    if (!step && st_passes != ST_LAPS * n) step = 4;
    if (!step && st_slow != 0) step = 5;
    if (!st_reaped(n0) && !step) step = 6;
    if (sched_configure(lim0, sched_stack_bytes()) != 0 && !step) step = 7;
    if (step) {
        /* the step first: a reader matches a test's first FAIL line */
        st_fail("smp stress: step ", (uint32_t)step);
        klog_u32("KTEST", SEV_FAIL, "smp stress: passes ", st_passes, LOG_COLOR_VALUE, "");
        klog_u32("KTEST", SEV_FAIL, "smp stress: slow waits ", st_slow, LOG_COLOR_VALUE, "");
        return 0;
    }
    return 1;
}

/* ---- the profile ----
 * In the hardware report's form (diag/hwfmt.h), a report of its own after
 * the suite, whose first line is `profile`, so `cxk hw diff --profile` sets
 * one machine's beside another's (docs/hardware/CX_HW_REPORT.md §8):
 *   @hw profile <cpus> <tsc-hz> <stress-ticks> <passes>
 *   @hw lock sched boot   <cpu> <acquired> <contended> <spins> <wait-ticks> <max-hold>
 *   @hw lock sched stress <cpu> <acquired> <contended> <spins> <wait-ticks> -
 * `boot` is everything counted since the CPU started; `stress` the stress
 * test's share alone (the longest hold is not kept per window). */
static char prof[4096];

static void lock_line(struct hw_out *o, const char *scope, unsigned cpu,
                      const struct lockstat *s, const struct lockstat *base) {
    hw_line(o, "lock");
    hw_s(o, "sched");
    hw_s(o, scope);
    hw_u(o, cpu);
    hw_u(o, s->acquired  - (base ? base->acquired  : 0));
    hw_u(o, s->contended - (base ? base->contended : 0));
    hw_u(o, s->spins     - (base ? base->spins     : 0));
    hw_u(o, s->wait_tsc  - (base ? base->wait_tsc  : 0));
    if (base) hw_s(o, "-");
    else      hw_u(o, s->max_hold);
    hw_done(o);
}

void ktest_profile_report(void) {
    struct hw_out o;
    unsigned n = cpu_count();
    hw_open(&o, prof, sizeof prof);
    hw_line(&o, "profile");
    hw_u(&o, n);
    hw_u(&o, timer_tsc_hz());
    hw_u(&o, st_ran ? st_ticks : 0);
    hw_u(&o, st_ran ? st_passes : 0);
    hw_done(&o);
    for (unsigned i = 0; i < n; i++) {
        struct lockstat now = sched_stat_of(i);
        lock_line(&o, "boot", i, &now, 0);
    }
    for (unsigned i = 0; i < n; i++) {
        static const struct lockstat none;
        lock_line(&o, "stress", i, st_ran ? &st_after[i] : &none, st_ran ? &st_before[i] : &none);
    }
    uint32_t len = hw_close(&o);
    for (uint32_t at = 0; at < len; ) {
        uint32_t e = at;
        while (e < len && prof[e] != '\n') e++;
        log_lock();
        serial_puts("\r\n");
        for (uint32_t i = at; i < e; i++) serial_putc(prof[i]);
        serial_puts("\r\n");
        log_unlock();
        at = e + 1;
    }
}

#else   /* no hooks: nothing is counted, and the tests fail rather than skip */
int  ktest_lockstat_count(void) { return st_fail("lockstat count: step ", 0); }
int  ktest_smp_stress(void)     { return st_fail("smp stress: step ", 0); }
void ktest_profile_report(void) {}
#endif
