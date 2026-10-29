// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kwait.c */
/* Aurora Tejeda / CATX Systems */
/* Signals and the single-object wait: see kwait.h. */
#include "object/kwait.h"
#include "sync/spinlock.h"
#include "cpu/sched.h"
#include "drivers/char/timer.h"
#include "cxk_abi.h"

_Static_assert(KWAIT_FOREVER == WAIT_FOREVER, "kwait.h's forever must be the ABI's");

static spinlock_t wait_lock = SPINLOCK_INIT;

void kobj_signal(struct kobj *o, uint32_t bits) {
    uint32_t f = spin_lock(&wait_lock);
    uint32_t s = atomic_load(&o->signals);
    atomic_store(&o->signals, s | bits);   /* written only under the lock */
    for (struct kwaiter *w = o->waiters; w; w = w->next) thread_unblock(w->tid);
    spin_unlock(&wait_lock, f);
}

int kobj_wait(struct kobj *o, uint32_t want, uint32_t timeout_ms, uint32_t *seen) {
    uint32_t deadline = timer_ticks() + timeout_ms;   /* meaningless for FOREVER */
    struct kwaiter me = { .tid = thread_current_id(), .next = 0 };
    uint32_t f = spin_lock(&wait_lock);
    uint32_t s = atomic_load(&o->signals);
    while (!(s & want)) {
        uint32_t ms = 0;
        if (timeout_ms != KWAIT_FOREVER) {
            uint32_t now = timer_ticks();
            if (!timer_tick_before(now, deadline)) break;   /* the time is up */
            ms = deadline - now;                             /* >= 1: 0 would be forever */
        }
        /* In the list and BLOCKED before the lock goes, so a signal set from
           here on finds this thread and wakes it, even one set before it has
           switched away. Woken early (another bit, or nothing), it looks again
           with what is left of the time. */
        me.next = o->waiters;
        o->waiters = &me;
        thread_block_prepare(ms);
        spin_unlock(&wait_lock, f);
        thread_block_finish();
        f = spin_lock(&wait_lock);
        struct kwaiter **pp = &o->waiters;
        while (*pp != &me) pp = &(*pp)->next;
        *pp = me.next;
        s = atomic_load(&o->signals);
    }
    spin_unlock(&wait_lock, f);
    if (seen) *seen = s;
    return (s & want) ? E_OK : E_TIMED_OUT;
}

uint32_t kobj_waiters(struct kobj *o) {
    uint32_t n = 0;
    uint32_t f = spin_lock(&wait_lock);
    for (struct kwaiter *w = o->waiters; w; w = w->next) n++;
    spin_unlock(&wait_lock, f);
    return n;
}
