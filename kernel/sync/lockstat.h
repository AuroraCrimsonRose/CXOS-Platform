// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/sync/lockstat.h */
/* Aurora Tejeda / CATX Systems */
/*
 * A counted lock's contention, per CPU (roadmap 1.9f, docs/kernel/CX_SMP.md
 * §4a): what decides whether the one run order under sched_lock stays or
 * gives way to a run queue per CPU. Counted only in test-hook builds
 * (spin_lock_counted, sync/spinlock.h), into the taking CPU's own record
 * (cpu/percpu.h), with interrupts off, so no count needs an atomic.
 *
 * The hold is from the take to the give on the same CPU. A spinlock held
 * across a thread switch (sched_lock) is given back by the thread switched
 * to, which runs on the CPU the take was made on, so `since` is the CPU's
 * and not the lock's. As Haiku's spinlock contention counting
 * (src/system/kernel/smp.cpp, B_DEBUG_SPINLOCK_CONTENTION) and Linux's
 * lock_stat (contentions, waittime, holdtime), without the per-site table.
 */

#ifndef CXK_LOCKSTAT_H
#define CXK_LOCKSTAT_H

#include <stdint.h>

struct lockstat {
    uint64_t acquired;    /* takes */
    uint64_t contended;   /* takes that found it held */
    uint64_t spins;       /* looks at the word while it was held */
    uint64_t wait_tsc;    /* TSC ticks spent spinning for it */
    uint64_t max_hold;    /* the longest hold, TSC ticks, take to give */
    uint64_t since;       /* the TSC at the take that holds it now */
};

/* The locks counted: one record each in every struct cpu. */
#define LOCKSTAT_SCHED  0u
#define LOCKSTAT_N      1u

#endif
