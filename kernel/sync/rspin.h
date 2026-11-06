// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/sync/rspin.h */
/* Aurora Tejeda / CATX Systems */
/*
 * A spinlock its holder may take again (1.9e3).
 *
 * For a layer whose public calls call each other: CXFS's resolve reads
 * entries, a truncate writes one, and every caller outside it (the file
 * syscalls, the loader, the crash log, the self-tests) comes in at any of
 * them. Each public call takes the lock; a nested one finds its own CPU
 * already holding it and only counts. Serenity's Mutex (Kernel/Locking/
 * Mutex.cpp, `lock`: the holder locking again adds to m_times_locked) and
 * Haiku's recursive_lock (src/system/kernel/locks/lock.cpp,
 * `recursive_lock_lock`: the holder adds to `recursion`) work the same
 * way, except that theirs sleep and this one spins.
 *
 * It spins because what it covers never blocks: the disk drivers poll
 * their devices, and a syscall's whole body already runs with interrupts
 * off. The holder is a CPU, not a thread: interrupts are off from the
 * first take to the last give back, so the thread cannot switch, and the
 * CPU stands for it. Another CPU spins on the inner spinlock, serving
 * shootdowns while it does (spinlock.h); only the outermost take is
 * checked against the lock order.
 *
 * `broken` is the crash path's: kpanic takes these in whatever state it
 * finds them (it writes the crash record to disk), so once broken every
 * take and give back does nothing.
 */
#ifndef CXK_RSPIN_H
#define CXK_RSPIN_H

#include <stdint.h>
#include "sync/spinlock.h"
#include "cpu/percpu.h"
#include "panic.h"

struct rspin {
    spinlock_t        l;
    volatile uint32_t owner;   /* the holding CPU's index + 1, 0 for none */
    uint32_t          depth;   /* takes by the owner not yet given back */
    uint32_t          flags;   /* the interrupt state before the first take */
    volatile uint32_t broken;  /* the crash path's: no lock any more */
};

#define RSPIN_INIT(rank) { SPINLOCK_INIT(rank), 0, 0, 0, 0 }

/* Only the owner writes its own index here, with interrupts off, and
   clears it before the lock goes: another CPU reads 0 or someone else's,
   never its own. */
static inline int rspin_held(const struct rspin *r) {
    return __atomic_load_n(&r->owner, __ATOMIC_RELAXED) == cpu_this()->index + 1;
}

static inline void rspin_lock(struct rspin *r) {
    if (__atomic_load_n(&r->broken, __ATOMIC_RELAXED)) return;
    uint32_t f = arch_irq_save();
    if (rspin_held(r)) { r->depth++; return; }   /* interrupts were off: f is "off" */
    (void)spin_lock(&r->l);                     /* off already, so nothing to keep */
    r->owner = cpu_this()->index + 1;
    r->depth = 1;
    r->flags = f;
}

static inline void rspin_unlock(struct rspin *r) {
    if (__atomic_load_n(&r->broken, __ATOMIC_RELAXED)) return;
    if (!rspin_held(r)) kpanic("rspin: given back by a CPU that does not hold it");
    if (--r->depth) return;
    uint32_t f = r->flags;
    __atomic_store_n(&r->owner, 0u, __ATOMIC_RELAXED);
    spin_unlock(&r->l, f);
}

static inline void rspin_break(struct rspin *r) {
    __atomic_store_n(&r->broken, 1u, __ATOMIC_RELAXED);
}

#endif
