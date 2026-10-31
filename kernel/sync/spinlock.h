// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/sync/spinlock.h */
/* Aurora Tejeda / CATX Systems */
/*
 * A spinlock that saves and restores the interrupt state.
 *
 * Taking one disables interrupts on this CPU and spins on the lock word;
 * releasing it puts the interrupt flag back as it was. On one CPU the spin
 * never waits (interrupts are off, so nothing else on this CPU runs), which
 * makes it exactly today's "interrupts off" critical section. On several CPUs
 * the same code is a real lock. That is the point of introducing it now: SMP
 * becomes a change to where locks are already taken, not a hunt for every
 * place that assumed one CPU.
 *
 * Saved and restored rather than cli/sti, for the reason sched.c gives: a
 * caller that already had interrupts off must get them back off.
 *
 * Built for the host tests (CXK_HOST_TEST), the interrupt part is empty
 * (arch/cpu_ops.h) and the lock word is the whole lock, so the logic above it
 * is testable.
 */
#ifndef CXK_SPINLOCK_H
#define CXK_SPINLOCK_H

#include <stdint.h>
#include "arch/cpu_ops.h"
#ifndef CXK_HOST_TEST
#include "config.h"
#endif

/* With the test hooks built in, how many spinlocks are held: a race point
   reached holding one would park the lock with it (diag/testhook.h).
   Interrupts being off is no sign of one, since a syscall's whole body
   runs with them off. One count for the machine, which is one CPU's until
   SMP gives each CPU its own. */
#if CXK_TEST_HOOKS && !defined(CXK_HOST_TEST)
extern volatile uint32_t spin_held;
#define SPIN_HELD_ADD(n) (spin_held += (n))
#else
#define SPIN_HELD_ADD(n) ((void)0)
#endif

typedef struct { volatile uint32_t locked; } spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline uint32_t spin_irq_save(void)          { return arch_irq_save(); }
static inline void     spin_irq_restore(uint32_t f) { arch_irq_restore(f); }
static inline void     spin_relax(void)             { arch_cpu_relax(); }

/* Take the lock with interrupts off. Returns the flags to give back. */
static inline uint32_t spin_lock(spinlock_t *l) {
    uint32_t f = spin_irq_save();
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) spin_relax();
    SPIN_HELD_ADD(1u);
    return f;
}

static inline void spin_unlock(spinlock_t *l, uint32_t f) {
    SPIN_HELD_ADD((uint32_t)-1);
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
    spin_irq_restore(f);
}

#endif
