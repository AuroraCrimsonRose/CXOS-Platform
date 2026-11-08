// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/sync/lockorder.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The lock order: one rank per spinlock, and a checker.
 *
 * Every spinlock_t is made with a rank from the table below (SPINLOCK_INIT,
 * spin_init). A lock may be taken only while every lock this CPU holds has a
 * lower rank: the table is the order, outermost first. Two locks of the same
 * rank are never held together (no two htabs, no two caches). Each rank is a
 * lock class, as Serenity's LockRank (Kernel/Locking/LockRank.h) and WRK's
 * numbered queued spinlocks (public/sdk/inc/ntkeapi.h) are; unlike
 * Serenity's bitmask, a rank here is a position, so the checker compares
 * numbers and holds a short list of what is held, for the panic message.
 *
 * Built in with the test hooks (CXK_LOCK_CHECK follows CXK_TEST_HOOKS).
 * Taking a lock out of order, releasing one not held, holding more than
 * LOCKORDER_DEPTH, or switching threads with any held but the scheduler's
 * (which goes across the switch, and must) is a panic naming the
 * locks and where each was taken. What is held is each CPU's own
 * (struct cpu, cpu/percpu.h). The check runs before the spin, so an
 * inversion is caught on the first run that nests the two, not only on the
 * run that deadlocks. Every pair that does nest is recorded
 * (lockorder_seen), so a self-test can show the checker saw it.
 *
 * The pure part (the check, the held list, the record) builds for the host
 * tests; the panic and the per-CPU state are the kernel's.
 * Design: docs/kernel/CX_LOCK_ORDER.md.
 */
#ifndef CXK_LOCKORDER_H
#define CXK_LOCKORDER_H

#include <stdint.h>

/* The order, outermost first. A new lock goes where the locks it is taken
   under and the locks taken under it put it; CX_LOCK_ORDER.md gives the
   pairs each place rests on, read off a boot. */
enum lock_rank {
    LR_NONE = 0,       /* never valid: a lock made without a rank */
    LR_STORAGE,        /* storage/hardware/disk.c, CXFS and the disks (1.9e3) */
    LR_IPC,            /* cpu/ipc.c ipc_lock          > htab, kwait */
    LR_FUTEX,          /* cpu/futex.c futex_lock      > vmregion, pids */
    LR_HTAB,           /* object/htab.c, one per handle table */
    LR_VMREGION,       /* memman/vmregion.c, per space > pids, kmem, pagedb */
    LR_PIDS,           /* cpu/proc.c pids_lock        > ocache, ocache-slots, pagedb */
    LR_KMEM,           /* object/kmem.c, per object   > pagedb */
    LR_JOB,            /* cpu/job.c jobs_lock: the tree > kwait (1.9e4) */
    LR_KTIMER,         /* object/ktimer.c armed_lock  > kwait */
    LR_KWAIT,          /* object/kwait.c wait_lock     > sched */
    LR_SYSFILE,        /* cpu/sysfile.c files_lock    > sched, ocache (1.9e3) */
    LR_INPUT,          /* drivers/input/keyboard.c kbd_lock  > sched (1.9e4) */
    LR_PROC,           /* cpu/proc.c proc_lock: counts, end, stop > sched (1.9e4) */
    LR_SCHED,          /* cpu/sched.c sched_lock, held across a switch */
    LR_RANDOM,         /* drivers/char/random.c */
    LR_HEAP,           /* memman/heap.c heap_lock     > pagedb */
    LR_OCACHE,         /* memman/ocache.c, per cache  > ocache-slots, pagedb */
    LR_OCACHE_SLOTS,   /* memman/ocache.c slots_lock */
    LR_DMA,            /* memman/dma.c */
    LR_KSTACK,         /* memman/kstack.c slots_lock: nothing under it */
    LR_PAGEDB,         /* memman/pagedb.c */
    LR_CMOS,           /* platform/pc/rtc.c: the CMOS index and data ports (1.9e3) */
    LR_DEVTREE,        /* drivers/devtree.c: the node table; nothing under it but the log (2.1) */
    LR_LOG,            /* drivers/char/console.c: the screen, its serial tee
                          and the system log's ring, under any lock (1.9e3) */
    LR_COUNT
};

#define LOCKORDER_DEPTH 8   /* locks one CPU may hold at once */

const char *lockorder_name(uint32_t rank);

/* What one CPU holds, in the order taken. */
struct lockorder_held {
    uint32_t n;
    struct { const void *lock; uint32_t rank; uintptr_t at; } e[LOCKORDER_DEPTH];
};

enum {
    LO_OK = 0,
    LO_UNRANKED,      /* rank LR_NONE or past the table */
    LO_ORDER,         /* a held lock's rank is not lower */
    LO_DEPTH,         /* the list is full */
    LO_NOT_HELD,      /* released but not in the list */
    LO_SWITCH,        /* a thread switch with a lock held but the sched lock (kernel only) */
    LO_SWITCH_UNLOCKED, /* a thread switch without the sched lock (kernel only) */
};

/* The pure part. lockorder_take checks and, if all is well, adds the lock
   and records the pair (highest held rank, rank) in seen; on LO_ORDER *who
   is the index of the held entry in the way. lockorder_give removes the
   lock wherever it sits (release order is free). */
int lockorder_take(struct lockorder_held *h, uint32_t seen[LR_COUNT],
                   const void *lock, uint32_t rank, uintptr_t at, uint32_t *who);
int lockorder_give(struct lockorder_held *h, const void *lock);

#if !defined(CXK_HOST_TEST)
#include "config.h"
#ifndef CXK_LOCK_CHECK
#define CXK_LOCK_CHECK CXK_TEST_HOOKS
#endif
#if CXK_LOCK_CHECK
/* Bit j of lockorder_seen[i]: rank j was taken while i was the highest rank
   held, on any CPU: one record for the machine, set atomically. */
extern uint32_t lockorder_seen[LR_COUNT];
void lockorder_acquire(const void *lock, uint32_t rank, uintptr_t at);
void lockorder_release(const void *lock);
void lockorder_assert_none(const char *where);   /* panics if any is held */
/* At a thread switch: the sched lock, held across it, and nothing else.
   Armed, a failure leaves only the sched lock listed, for the thread
   switched to to give back. */
void lockorder_assert_switch(const void *sched_lock, const char *where);
uint32_t lockorder_depth(void);
void lockorder_stop(void);                       /* the panic path: check no more */
void lockorder_report(void);                     /* the seen pairs, to the log */
/* For the self-tests: while armed, a failure is counted, not a panic (an
   out-of-order lock is still listed; a switch clears the list). Disarm
   returns the first failure's LO_ code and, in *count, how many. */
void lockorder_test_arm(void);
uint32_t lockorder_test_disarm(uint32_t *count);
#endif
#endif

#endif
