// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kpipe.c */
/* Aurora Tejeda / CATX Systems */
/* Pipes: see kpipe.h. */
#include "object/kpipe.h"
#include "object/kwait.h"
#include "memman/ocache.h"
#include "memman/heap.h"
#include "job.h"
#include "usermode.h"
#include "cxk_abi.h"
#include "panic.h"

_Static_assert(KOBJ_PIPE == OBJ_TYPE_PIPE, "object_basic.kind is the kernel type");
_Static_assert(PIPE_SIZE_MIN >= PIPE_ATOMIC,
               "a write of PIPE_ATOMIC bytes must fit, or it waits for room that never comes");

static struct ocache pipe_cache;

/* Each end's signals from the ring's state. Under the wait lock: the bytes and
   the signals change together, so a waiter that saw one finds the other. */
static void update_locked(struct kpipe *p) {
    if (p->count || p->wr_closed) kobj_signal_locked(&p->rd.hdr, SIGNAL_PIPE_READABLE);
    else                          kobj_signal_clear_locked(&p->rd.hdr, SIGNAL_PIPE_READABLE);
    if (p->size - p->count >= PIPE_ATOMIC || p->rd_closed)
        kobj_signal_locked(&p->wr.hdr, SIGNAL_PIPE_WRITABLE);
    else
        kobj_signal_clear_locked(&p->wr.hdr, SIGNAL_PIPE_WRITABLE);
    if (p->wr_closed) kobj_signal_locked(&p->rd.hdr, SIGNAL_PIPE_PEER_CLOSED);
    if (p->rd_closed) kobj_signal_locked(&p->wr.hdr, SIGNAL_PIPE_PEER_CLOSED);
}

/* Nobody can reach this end any more: the other end learns so. A thread still
   blocked on this end (another thread closed the handle) holds a reference
   and goes on as it was. */
static void pipe_last_handle(struct kobj *o) {
    struct kpipe_end *e = (struct kpipe_end *)o;
    uint32_t f = kwait_lock();
    if (e->write) e->p->wr_closed = 1;
    else          e->p->rd_closed = 1;
    update_locked(e->p);
    kwait_unlock(f);
}

/* Each end holds the pair; the second to go frees it. */
static void pipe_destroy(struct kobj *o) {
    struct kpipe *p = ((struct kpipe_end *)o)->p;
    if (atomic_fetch_sub(&p->ends, 1) != 1) return;
    for (uint32_t i = 0; i < p->size; i++) p->ring[i] = 0;   /* what was written stays private */
    kfree(p->ring);
    job_uncharge_mem(p->job, p->size);
    job_put(p->job);
    ocache_free(&pipe_cache, p);
}

static const struct kobj_ops pipe_ops = {
    .name          = "pipe",
    .rights_valid  = HRIGHT_READ | HRIGHT_WRITE,
    .signals_valid = SIGNAL_PIPE_READABLE | SIGNAL_PIPE_WRITABLE | SIGNAL_PIPE_PEER_CLOSED,
    .transfer      = 1,
    .last_handle   = pipe_last_handle,
    .destroy       = pipe_destroy,
};

void kpipe_sys_init(void) {
    if (ocache_init(&pipe_cache, "pipe", sizeof(struct kpipe)) != 0 ||
        kobj_type_register(KOBJ_PIPE, &pipe_ops) != 0)
        kpanic("kpipe_sys_init: no pipe cache");
}

struct kpipe *kpipe_create(uint32_t size, struct job *job, int *rc) {
    struct kpipe *p = ocache_alloc(&pipe_cache);   /* zeroed: empty, both ends open */
    if (!p) { *rc = E_NO_MEMORY; return 0; }
    *rc = job_charge_mem(job, size);
    if (*rc != E_OK) { ocache_free(&pipe_cache, p); return 0; }
    p->ring = kmalloc(size);
    if (!p->ring) { job_uncharge_mem(job, size); ocache_free(&pipe_cache, p); *rc = E_NO_MEMORY; return 0; }
    if (kobj_init(&p->rd.hdr, KOBJ_PIPE) != 0 || kobj_init(&p->wr.hdr, KOBJ_PIPE) != 0)
        kpanic("kpipe_create: pipe type not registered");
    kobj_ref(&job->hdr);
    p->job  = job;
    p->size = size;
    p->rd.p = p;
    p->wr.p = p; p->wr.write = 1;
    atomic_store(&p->ends, 2);
    uint32_t f = kwait_lock();
    update_locked(p);   /* the write end starts writable */
    kwait_unlock(f);
    return p;
}

int kpipe_read(struct kpipe_end *e, uint64_t ubuf, uint32_t n) {
    struct kpipe *p = e->p;
    uint8_t bounce[PIPE_ATOMIC];
    uint32_t done = 0;
    while (done < n) {
        uint32_t f = kwait_lock();
        if (!p->count) {
            int eof = p->wr_closed;
            kwait_unlock(f);
            if (done || eof) break;   /* what there was, or the end of the stream */
            /* READABLE is clear here, and set under the same lock by the
               write that ends the wait, so the wake cannot be missed. */
            int rc = kobj_wait(&e->hdr, SIGNAL_PIPE_READABLE, KWAIT_NEVER, 0);
            if (rc != 0) return rc;
            continue;
        }
        uint32_t k = n - done;
        if (k > p->count)   k = p->count;
        if (k > PIPE_ATOMIC) k = PIPE_ATOMIC;
        for (uint32_t i = 0; i < k; i++) bounce[i] = p->ring[(p->head + i) % p->size];
        p->head   = (p->head + k) % p->size;
        p->count -= k;
        update_locked(p);
        kwait_unlock(f);
        /* The caller checked the whole buffer writable, and pages arrive at
           that check; failing here means the mapping changed since, and the
           bytes taken are lost with the call. */
        if (user_copy_out(ubuf + done, bounce, k) != 0) return E_BAD_ADDRESS;
        done += k;
    }
    return (int)done;
}

int kpipe_write(struct kpipe_end *e, uint64_t ubuf, uint32_t n) {
    struct kpipe *p = e->p;
    uint8_t bounce[PIPE_ATOMIC];
    uint32_t done = 0;
    while (done < n) {
        uint32_t k = n - done;
        if (k > PIPE_ATOMIC) k = PIPE_ATOMIC;
        if (user_copy_in(bounce, ubuf + done, k) != 0) return done ? (int)done : E_BAD_ADDRESS;
        uint32_t put = 0;
        while (put < k) {
            uint32_t f = kwait_lock();
            if (p->rd_closed) {
                kwait_unlock(f);
                done += put;
                return done ? (int)done : E_PEER_CLOSED;
            }
            uint32_t room = p->size - p->count;
            /* A write of PIPE_ATOMIC bytes or fewer waits for room for all of
               it; a longer one puts what fits. Either way the wait is for
               WRITABLE, PIPE_ATOMIC bytes free, which is clear whenever room
               is short of `need`: the loop never spins. */
            uint32_t need = (n <= PIPE_ATOMIC) ? k - put : 1;
            if (room < need) {
                kwait_unlock(f);
                int rc = kobj_wait(&e->hdr, SIGNAL_PIPE_WRITABLE, KWAIT_NEVER, 0);
                if (rc != 0) { done += put; return done ? (int)done : rc; }
                continue;
            }
            uint32_t m = k - put;
            if (m > room) m = room;
            uint32_t tail = (p->head + p->count) % p->size;
            for (uint32_t i = 0; i < m; i++) p->ring[(tail + i) % p->size] = bounce[put + i];
            p->count += m;
            put      += m;
            update_locked(p);
            kwait_unlock(f);
        }
        done += k;
    }
    return (int)done;
}

uint32_t kpipe_count(const struct kpipe *p) { return p->count; }
