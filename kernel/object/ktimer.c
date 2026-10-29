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
    .destroy       = timer_destroy,
};

void ktimer_sys_init(void) {
    if (ocache_init(&timer_cache, "timer", sizeof(struct ktimer)) != 0 ||
        kobj_type_register(KOBJ_TIMER, &timer_ops) != 0)
        kpanic("ktimer_sys_init: no timer cache");
}

struct ktimer *ktimer_create(void) {
    struct ktimer *t = ocache_alloc(&timer_cache);   /* zeroed: disarmed */
    if (!t) return 0;
    if (kobj_init(&t->hdr, KOBJ_TIMER) != 0) { ocache_free(&timer_cache, t); return 0; }
    return t;
}

void ktimer_set(struct ktimer *t, uint64_t deadline) {
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
    }
    spin_unlock(&armed_lock, f);
}

void ktimer_cancel(struct ktimer *t) {
    uint32_t f = spin_lock(&armed_lock);
    disarm(t);
    kobj_signal_clear(&t->hdr, SIGNAL_TIMER_FIRED);
    spin_unlock(&armed_lock, f);
}

void ktimer_tick(void) {
    /* Every millisecond: nothing armed costs one load, not the lock. A list
       armed as this reads it is looked at again on the next tick. */
    if (!__atomic_load_n(&armed, __ATOMIC_RELAXED)) return;
    uint32_t f = spin_lock(&armed_lock);
    uint64_t now = timer_now_ns();
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
