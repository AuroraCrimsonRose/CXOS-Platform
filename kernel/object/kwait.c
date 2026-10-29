// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kwait.c */
/* Aurora Tejeda / CATX Systems */
/* Signals and the single-object wait: see kwait.h. */
#include "object/kwait.h"
#include "object/kqueue.h"   /* kqueue_signalled */
#include "sync/spinlock.h"
#include "cpu/sched.h"
#include "drivers/char/timer.h"
#include "cxk_abi.h"

_Static_assert(KWAIT_NEVER == DEADLINE_NEVER, "kwait.h's never must be the ABI's");

_Static_assert(KWAIT_NEVER == TIMER_NEVER, "kwait.h's never must be the clock's");

static spinlock_t wait_lock = SPINLOCK_INIT;

uint32_t kwait_lock(void)          { return spin_lock(&wait_lock); }
void     kwait_unlock(uint32_t f)  { spin_unlock(&wait_lock, f); }

void kobj_signal_locked(struct kobj *o, uint32_t bits) {
    uint32_t s = atomic_load(&o->signals) | bits;
    atomic_store(&o->signals, s);   /* written only under the lock */
    for (struct kwaiter *w = o->waiters; w; w = w->next) thread_unblock(w->tid);
    if (o->qregs) kqueue_signalled(o, s);
}

void kobj_signal_clear_locked(struct kobj *o, uint32_t bits) {
    atomic_store(&o->signals, atomic_load(&o->signals) & ~bits);
}

void kobj_signal(struct kobj *o, uint32_t bits) {
    uint32_t f = spin_lock(&wait_lock);
    kobj_signal_locked(o, bits);
    spin_unlock(&wait_lock, f);
}

void kobj_signal_clear(struct kobj *o, uint32_t bits) {
    uint32_t f = spin_lock(&wait_lock);
    kobj_signal_clear_locked(o, bits);
    spin_unlock(&wait_lock, f);
}

int kobj_wait(struct kobj *o, uint32_t want, uint64_t deadline, uint32_t *seen) {
    struct kwaiter me = { .tid = thread_current_id(), .next = 0 };
    uint32_t f = spin_lock(&wait_lock);
    uint32_t s = atomic_load(&o->signals);
    int killed = 0;   /* woken by proc_kill (cpu/sched.h), or not blocked by it */
    while (!(s & want)) {
        /* The block ends at the deadline itself, on the same clock, so the
           time is up the first time this looks after it. */
        if (deadline < KWAIT_NEVER && timer_now_ns() >= deadline) break;
        /* In the list and BLOCKED before the lock goes, so a signal set from
           here on finds this thread and wakes it, even one set before it has
           switched away. Woken early (another bit, or nothing), it looks again
           with what is left of the time. */
        me.next = o->waiters;
        o->waiters = &me;
        thread_block_prepare_until(deadline < KWAIT_NEVER ? deadline : TIMER_NEVER);
        spin_unlock(&wait_lock, f);
        thread_block_finish();
        f = spin_lock(&wait_lock);
        struct kwaiter **pp = &o->waiters;
        while (*pp != &me) pp = &(*pp)->next;
        *pp = me.next;
        s = atomic_load(&o->signals);
        if (!(s & want) && thread_killed()) { killed = 1; break; }
    }
    spin_unlock(&wait_lock, f);
    if (seen) *seen = s;
    return (s & want) ? E_OK : killed ? E_CANCELED : E_TIMED_OUT;
}

uint32_t kobj_waiters(struct kobj *o) {
    uint32_t n = 0;
    uint32_t f = spin_lock(&wait_lock);
    for (struct kwaiter *w = o->waiters; w; w = w->next) n++;
    spin_unlock(&wait_lock, f);
    return n;
}
