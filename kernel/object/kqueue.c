// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kqueue.c */
/* Aurora Tejeda / CATX Systems */
/* Queue objects: see kqueue.h. Every list here is under the wait lock. */
#include "object/kqueue.h"
#include "object/kwait.h"
#include "memman/ocache.h"
#include "sync/atomic.h"
#include "panic.h"

_Static_assert(KOBJ_QUEUE == OBJ_TYPE_QUEUE, "object_basic.kind is the kernel type");

enum { KQ_ARMED = 1, KQ_READY = 2, KQ_IDLE = 3 };

static struct ocache queue_cache, reg_cache;
static atomic_u32    regs_live;

/* Wait lock held. */
static void make_ready(struct kqreg *r, uint32_t s) {
    struct kqueue *q = r->q;
    r->state = KQ_READY;
    r->seen  = s;
    r->rnext = 0;
    if (q->ready_tail) q->ready_tail->rnext = r; else q->ready = r;
    q->ready_tail = r;
    kobj_signal_locked(&q->hdr, SIGNAL_QUEUE_READY);
}

/* Wait lock held. A signal set while it was idle readies it now. */
static void arm(struct kqreg *r) {
    if (r->state != KQ_IDLE) return;
    uint32_t s = atomic_load(&r->obj->signals);
    if (s & r->want) make_ready(r, s);
    else r->state = KQ_ARMED;
}

/* Wait lock held. Off the ready list, if on it. */
static void unready(struct kqreg *r) {
    struct kqueue *q = r->q;
    if (r->state != KQ_READY) return;
    struct kqreg *prev = 0;
    for (struct kqreg *x = q->ready; x; prev = x, x = x->rnext) {
        if (x != r) continue;
        if (prev) prev->rnext = r->rnext; else q->ready = r->rnext;
        if (q->ready_tail == r) q->ready_tail = prev;
        break;
    }
    r->rnext = 0;
    r->state = KQ_IDLE;
    if (!q->ready) kobj_signal_clear_locked(&q->hdr, SIGNAL_QUEUE_READY);
}

/* Wait lock held. Out of its object's list. */
static void unlink_obj(struct kqreg *r) {
    for (struct kqreg **pp = &r->obj->qregs; *pp; pp = &(*pp)->onext)
        if (*pp == r) { *pp = r->onext; break; }
    r->onext = 0;
}

/* Wait lock not held: dropping the object's reference may destroy it, and
   its destroy hook may take a lock ordered before this one (ktimer.c). */
static void reg_free(struct kqreg *r) {
    kobj_unref(r->obj);
    ocache_free(&reg_cache, r);
    atomic_fetch_sub(&regs_live, 1);
}

static struct kqreg *find(struct kqueue *q, uint64_t key) {
    for (struct kqreg *r = q->regs; r; r = r->qnext) if (r->key == key) return r;
    return 0;
}

/* The last reference: nobody can register on it or wait on it any more. */
static void queue_destroy(struct kobj *o) {
    struct kqueue *q = (struct kqueue *)o;
    uint32_t f = kwait_lock();
    struct kqreg *all = q->regs;
    for (struct kqreg *r = all; r; r = r->qnext) unlink_obj(r);
    q->regs = q->ready = q->ready_tail = 0;
    q->nregs = 0;
    kwait_unlock(f);
    while (all) {
        struct kqreg *r = all;
        all = r->qnext;
        reg_free(r);
    }
    ocache_free(&queue_cache, q);
}

static const struct kobj_ops queue_ops = {
    .name          = "queue",
    .rights_valid  = HRIGHT_REGISTER,
    .signals_valid = SIGNAL_QUEUE_READY,
    .destroy       = queue_destroy,
};

void kqueue_sys_init(void) {
    if (ocache_init(&queue_cache, "queue", sizeof(struct kqueue)) != 0 ||
        ocache_init(&reg_cache, "queue reg", sizeof(struct kqreg)) != 0 ||
        kobj_type_register(KOBJ_QUEUE, &queue_ops) != 0)
        kpanic("kqueue_sys_init: no queue cache");
}

struct kqueue *kqueue_create(void) {
    struct kqueue *q = ocache_alloc(&queue_cache);   /* zeroed: empty */
    if (!q) return 0;
    if (kobj_init(&q->hdr, KOBJ_QUEUE) != 0) { ocache_free(&queue_cache, q); return 0; }
    return q;
}

int kqueue_register(struct kqueue *q, struct kobj *o, uint32_t want, uint64_t key) {
    /* A queue on a queue could hold itself, and readying one would signal
       inside the signal path. */
    if (o->type == KOBJ_QUEUE) return E_WRONG_TYPE;
    struct kqreg *r = ocache_alloc(&reg_cache);   /* before the lock: it may map pages */
    if (!r) return E_NO_MEMORY;
    uint32_t f = kwait_lock();
    int rc = find(q, key)                   ? E_ALREADY_EXISTS
           : q->nregs >= QUEUE_REGISTER_MAX ? E_NO_RESOURCES : E_OK;
    if (rc != E_OK) {
        kwait_unlock(f);
        ocache_free(&reg_cache, r);
        return rc;
    }
    kobj_ref(o);
    atomic_fetch_add(&regs_live, 1);
    r->q = q; r->obj = o; r->key = key; r->want = want; r->state = KQ_IDLE;
    r->qnext = q->regs; q->regs = r;
    r->onext = o->qregs; o->qregs = r;
    q->nregs++;
    arm(r);
    kwait_unlock(f);
    return E_OK;
}

int kqueue_arm(struct kqueue *q, uint64_t key) {
    uint32_t f = kwait_lock();
    struct kqreg *r = find(q, key);
    if (r) arm(r);
    kwait_unlock(f);
    return r ? E_OK : E_NOT_FOUND;
}

int kqueue_unregister(struct kqueue *q, uint64_t key) {
    uint32_t f = kwait_lock();
    struct kqreg *r = find(q, key);
    if (r) {
        unready(r);
        unlink_obj(r);
        struct kqreg **pp = &q->regs;
        while (*pp != r) pp = &(*pp)->qnext;
        *pp = r->qnext;
        q->nregs--;
    }
    kwait_unlock(f);
    if (!r) return E_NOT_FOUND;
    reg_free(r);
    return E_OK;
}

int kqueue_wait(struct kqueue *q, struct queue_event *out, uint32_t n, uint64_t deadline) {
    for (;;) {
        int rc = kobj_wait(&q->hdr, SIGNAL_QUEUE_READY, deadline, 0);
        if (rc != E_OK) return rc;
        uint32_t got = 0;
        uint32_t f = kwait_lock();
        while (got < n && q->ready) {
            struct kqreg *r = q->ready;
            out[got].key      = r->key;
            out[got].signals  = r->seen;
            out[got].reserved = 0;
            got++;
            unready(r);   /* the head: clears the signal with the last */
        }
        kwait_unlock(f);
        /* Another waiter took them between the wake and the lock: wait again
           with what is left of the time. */
        if (got) return (int)got;
    }
}

void kqueue_signalled(struct kobj *o, uint32_t s) {
    for (struct kqreg *r = o->qregs; r; r = r->onext)
        if (r->state == KQ_ARMED && (s & r->want)) make_ready(r, s);
}

uint32_t kqueue_reg_count(void) { return atomic_load(&regs_live); }
