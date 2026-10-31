// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/testhook.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The self-tests' hooks into the kernel (roadmap Phase 1, step 6;
 * safety/CX_SAFETY.md §7). Three of them:
 *
 *   Failing the Nth allocation. A test arms a set of allocation sites and a
 *   number N; the Nth call the ARMING THREAD makes at any of them fails, as
 *   if memory had run out, and every other call goes through. Only the
 *   arming thread counts, so a timer or a helper allocating meanwhile neither
 *   takes the failure nor moves N. A sweep runs one operation with N = 1, 2,
 *   ... until a run finishes without reaching N: every allocation the
 *   operation makes has then failed once, each on its own run.
 *
 *   Live counts. A snapshot of what the kernel holds (free and reserved
 *   pages, heap bytes and blocks, kernel stacks, threads, each object cache,
 *   each object type, and for one process and one job their handles, mapped
 *   bytes and charges). Taken before and after an operation that should
 *   leave nothing behind, two snapshots must be the same.
 *
 *   Race points. A named place in kernel code where a test can park the
 *   thread that reaches it (the test then acts, and releases it) or have that
 *   thread call a function of the test's (a close, a kill) right there. A
 *   point fires once per arming. It must be reached holding no spinlock: a
 *   point under one would park the lock with it, and is a panic
 *   (spin_held, sync/spinlock.h). Interrupts may be off, as they are for a
 *   syscall's whole body; a park yields, and each thread keeps its own.
 *
 * XNU names its injection points the same way, as stages with one armed at
 * a time (osfmk/kern/upsi.h, check_for_failure_injection in debug.c), there
 * to panic or hang at boot rather than to test a path. Serenity reads its
 * heap's numbers in one call (Kernel/Heap/kmalloc.cpp, get_kmalloc_stats).
 * No fault injection was found in WRK-v1.2 or Haiku's kernel by search; the
 * fail-the-Nth idea is Linux's fail_nth, from memory (not indexed).
 *
 * With CXK_TEST_HOOKS 0 (config.h) the sites compile to nothing, and the
 * functions a test calls report that the hooks are missing.
 */

#ifndef CXK_DIAG_TESTHOOK_H
#define CXK_DIAG_TESTHOOK_H

#include <stdint.h>
#ifndef CXK_HOST_TEST
#include "config.h"
#endif

/* ---- failing the Nth allocation ---- */

enum fault_site {
    FAULT_KMALLOC = 1u << 0,   /* kmalloc: NULL */
    FAULT_OCACHE  = 1u << 1,   /* ocache_alloc: NULL */
    FAULT_PMM     = 1u << 2,   /* pmm_alloc, _pages, _pages_low: 0 (never pmm_alloc_reserved) */
    FAULT_KSTACK  = 1u << 3,   /* kstack_alloc: 0 */
    FAULT_VMMAP   = 1u << 4,   /* vm_map_object, past its argument checks: E_NO_MEMORY */
    FAULT_ALL     = 0x1Fu
};

#if CXK_TEST_HOOKS && !defined(CXK_HOST_TEST)
extern volatile int fault_armed;
int fault_hit_slow(uint32_t site);
/* At an allocation site: 1 if this call is to fail. */
static inline int fault_hit(uint32_t site) {
    return fault_armed ? fault_hit_slow(site) : 0;
}
#else
static inline int fault_hit(uint32_t site) { (void)site; return 0; }
#endif

/* The current thread's Nth call (from 1) at any of `sites` fails. -1 if the
   hooks are not built in, or N is 0. Replaces any earlier arming. */
int      fault_arm(uint32_t sites, uint32_t nth);
/* Stop counting. Returns the calls counted at the armed sites. */
uint32_t fault_disarm(void);
/* Did the armed call fail (since the last fault_arm)? */
int      fault_fired(void);

/* ---- live counts ---- */

#define LIVE_CACHES 32

struct ocache;
struct job;

struct live_counts {
    uint32_t pages_free, pages_reserved;
    uint32_t heap_bytes, heap_blocks;
    uint32_t kstacks, threads, procs, jobs;
    uint32_t types[16];                    /* kobj_live per type, below KOBJ_TYPE_MAX */
    uint32_t ncache;
    struct { const struct ocache *c; uint32_t live; } cache[LIVE_CACHES];
    int      pid;                          /* -1: no process */
    uint32_t handles;
    uint64_t mapped;
    const struct job *job;                 /* NULL: no job */
    uint64_t job_mem;
    uint32_t job_threads, job_procs;
};

/* A snapshot; `pid` -1 and `job` NULL leave those parts out. */
void     live_take(struct live_counts *s, int pid, const struct job *job);
/* How many counts differ; with `log`, a line for each, naming it, so a leak
   says what leaked. */
uint32_t live_diff(const struct live_counts *before, const struct live_counts *after, int log);

/* ---- race points ---- */

enum race_point {
    RP_SELFTEST = 0,           /* reached only by the hooks' own self-test */
    RP_FUTEX_READ,             /* futex_wait: the word faulted in and keyed, not yet read */
    RP_FUTEX_WOKEN,            /* futex_wait: its block over, futex_lock not yet retaken */
    RP_COUNT
};

enum race_action {
    RACE_PARK = 1,             /* the thread waits there until race_release */
    RACE_CALL = 2              /* the thread calls fn(ctx) there, then goes on */
};

#if CXK_TEST_HOOKS && !defined(CXK_HOST_TEST)
extern volatile uint32_t race_armed_mask;
void race_point_slow(enum race_point p);
/* In kernel code, at the named place. */
static inline void race_point(enum race_point p) {
    if (race_armed_mask & (1u << p)) race_point_slow(p);
}
#else
static inline void race_point(enum race_point p) { (void)p; }
#endif

/* Arm `p`: the Nth arrival (from 1) of thread `tid` (-1: any thread) does
   `act`. -1 if the hooks are not built in or the arguments are bad. */
int  race_arm(enum race_point p, int tid, uint32_t nth, enum race_action act,
              void (*fn)(void *ctx), void *ctx);
/* Wait until a thread is parked at `p`, at most `ms`: its id, or -1. */
int  race_wait_parked(enum race_point p, uint32_t ms);
/* Let a parked thread go on; disarms `p` either way. */
void race_release(enum race_point p);
/* Arrivals at `p` since it was armed. */
uint32_t race_arrivals(enum race_point p);

#endif
