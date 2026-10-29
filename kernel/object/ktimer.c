// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/ktimer.c */
/* Aurora Tejeda / CATX Systems */
/* Timer objects: see ktimer.h. */
#include "object/ktimer.h"
#include "object/kwait.h"
#include "sync/spinlock.h"
#include "memman/ocache.h"
#include "drivers/char/timer.h"
#include "cxk_abi.h"
#include "panic.h"

_Static_assert(KOBJ_TIMER == OBJ_TYPE_TIMER, "object_basic.kind is the kernel type");
_Static_assert(TIMER_NEVER == DEADLINE_NEVER, "the clock's never is the ABI's");

static struct ocache  timer_cache;
static spinlock_t     armed_lock = SPINLOCK_INIT;
static struct ktimer *armed;   /* earliest deadline first */

/* armed_lock held. */
static void disarm(struct ktimer *t) {
    if (!t->armed) return;
    for (struct ktimer **pp = &armed; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    t->next  = 0;
    t->armed = 0;
}

/* No last_handle hook: a thread still waiting on it after the last handle
   closed (another thread of its process closed it) holds a reference, and
   the timer still fires for it. It leaves the list with the last reference. */
static void timer_destroy(struct kobj *o) {
    uint32_t f = spin_lock(&armed_lock);
    disarm((struct ktimer *)o);
    spin_unlock(&armed_lock, f);
    ocache_free(&timer_cache, o);
}

static const struct kobj_ops timer_ops = {
    .name          = "timer",
    .rights_valid  = HRIGHT_ARM,
    .signals_valid = SIGNAL_TIMER_FIRED,
    .transfer      = 1,
    .destroy       = timer_destroy,
};

void ktimer_sys_init(void) {
    if (ocache_init(&timer_cache, "timer", sizeof(struct ktimer)) != 0 ||
        kobj_type_register(KOBJ_TIMER, &timer_ops) != 0)
        kpanic("ktimer_sys_init: no timer cache");
}

struct ktimer *ktimer_create(uint64_t slack) {
    struct ktimer *t = ocache_alloc(&timer_cache);   /* zeroed: disarmed */
    if (!t) return 0;
    if (kobj_init(&t->hdr, KOBJ_TIMER) != 0) { ocache_free(&timer_cache, t); return 0; }
    t->slack = slack;
    return t;
}

/* The latest `t` may fire: its deadline and slack, held below never. */
static uint64_t latest(const struct ktimer *t) {
    uint64_t l = t->deadline + t->slack;
    return l < t->deadline || l > TIMER_NEVER ? TIMER_NEVER : l;
}

void ktimer_set(struct ktimer *t, uint64_t deadline) {
    uint64_t hint = TIMER_NEVER;
    uint32_t f = spin_lock(&armed_lock);
    disarm(t);
    kobj_signal_clear(&t->hdr, SIGNAL_TIMER_FIRED);
    if (deadline <= timer_now_ns()) {
        kobj_signal(&t->hdr, SIGNAL_TIMER_FIRED);
    } else {
        /* After every timer due no later: two for the same deadline fire in
           the order they were armed. */
        struct ktimer **pp = &armed;
        while (*pp && (*pp)->deadline <= deadline) pp = &(*pp)->next;
        t->deadline = deadline;
        t->next     = *pp;
        t->armed    = 1;
        *pp         = t;
        hint        = latest(t);
    }
    spin_unlock(&armed_lock, f);
    if (hint < TIMER_NEVER) timer_deadline_hint(hint);
}

void ktimer_cancel(struct ktimer *t) {
    uint32_t f = spin_lock(&armed_lock);
    disarm(t);
    kobj_signal_clear(&t->hdr, SIGNAL_TIMER_FIRED);
    spin_unlock(&armed_lock, f);
}

uint64_t ktimer_next_due(void) {
    uint64_t d = TIMER_NEVER;
    if (!__atomic_load_n(&armed, __ATOMIC_RELAXED)) return d;
    uint32_t f = spin_lock(&armed_lock);
    /* In deadline order, and a latest time is never before its deadline:
       past a deadline at or after the best so far, nothing can beat it. */
    for (struct ktimer *t = armed; t && t->deadline < d; t = t->next) {
        uint64_t l = latest(t);
        if (l < d) d = l;
    }
    spin_unlock(&armed_lock, f);
    return d;
}

void ktimer_tick(uint64_t now) {
    /* Nothing armed costs one load, not the lock. A timer armed as this
       reads it asked for an interrupt of its own (timer_deadline_hint). */
    if (!__atomic_load_n(&armed, __ATOMIC_RELAXED)) return;
    uint32_t f = spin_lock(&armed_lock);
    /* By deadline, not latest time: whatever interrupt comes, every timer
       whose deadline it has reached fires with it. That is the slack. */
    while (armed && armed->deadline <= now) {
        struct ktimer *t = armed;
        armed    = t->next;
        t->next  = 0;
        t->armed = 0;
        kobj_signal(&t->hdr, SIGNAL_TIMER_FIRED);
    }
    spin_unlock(&armed_lock, f);
}

uint32_t ktimer_armed_count(void) {
    uint32_t n = 0;
    uint32_t f = spin_lock(&armed_lock);
    for (struct ktimer *t = armed; t; t = t->next) n++;
    spin_unlock(&armed_lock, f);
    return n;
}
