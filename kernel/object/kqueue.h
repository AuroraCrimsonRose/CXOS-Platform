// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kqueue.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Queue objects (KOBJ_QUEUE; docs/kernel/CX_KERNEL_PLAN.md section 5): wait for
 * many objects at once without naming them on every wait.
 *
 * A registration names an object, the signals wanted from it and a key. It is
 * one-shot, as XNU's EV_DISPATCH knotes are (bsd/kern/kern_event.c,
 * knote_process): ARMED until one of its signals is set, then READY, on the
 * queue's ready list, until a wait takes it; then IDLE until re-armed. Arming
 * looks at the signals as they are, so one set while it was idle is not
 * missed, and a key is handed to one waiter, never to every thread waiting.
 *
 * Each object keeps its registrations (kobj.qregs), and kobj_signal readies
 * the armed ones under the wait lock (kwait.c), which covers every list here
 * too. A registration holds a reference on its object, as a handle does; the
 * queue drops them all when it goes. A queue is never registered on a queue,
 * and a process's handles close when it ends, so no cycle of references
 * outlives the processes that made it.
 */
#ifndef CXK_KQUEUE_H
#define CXK_KQUEUE_H

#include <stdint.h>
#include "object/kobj.h"
#include "cxk_abi.h"   /* struct queue_event */

struct kqueue;

struct kqreg {
    struct kqueue *q;
    struct kobj   *obj;      /* referenced while registered */
    uint64_t       key;
    uint32_t       want;     /* signals that ready it */
    uint32_t       seen;     /* obj's signals when it became ready */
    uint8_t        state;    /* KQ_ARMED, KQ_READY, KQ_IDLE (kqueue.c) */
    struct kqreg  *onext;    /* in obj->qregs */
    struct kqreg  *qnext;    /* in q->regs */
    struct kqreg  *rnext;    /* in q's ready list, oldest first */
};

struct kqueue {
    struct kobj   hdr;       /* first; KOBJ_QUEUE */
    struct kqreg *regs;
    struct kqreg *ready, *ready_tail;
    uint32_t      nregs;
};

void kqueue_sys_init(void);

/* A new, empty queue, with one reference for the caller (who installs a handle
   and drops it). 0 when out of memory. */
struct kqueue *kqueue_create(void);

/* Register `o` (the caller has checked `want` against its type) under `key`,
   armed. Takes its own reference on `o`. E_OK, E_WRONG_TYPE (a queue),
   E_ALREADY_EXISTS, E_NO_RESOURCES (QUEUE_REGISTER_MAX) or E_NO_MEMORY. */
int kqueue_register(struct kqueue *q, struct kobj *o, uint32_t want, uint64_t key);

/* Arm the registration under `key` if it is idle. E_OK or E_NOT_FOUND. */
int kqueue_arm(struct kqueue *q, uint64_t key);

/* Remove the registration under `key`, ready or not. E_OK or E_NOT_FOUND. */
int kqueue_unregister(struct kqueue *q, uint64_t key);

/* Wait until a key is ready or `deadline` (as kobj_wait takes it), then take up
   to `n` (1..QUEUE_EVENTS_MAX) ready keys into `out`, oldest first, leaving
   them idle. Returns how many, E_TIMED_OUT or E_CANCELED. The caller holds a
   reference on `q`. */
int kqueue_wait(struct kqueue *q, struct queue_event *out, uint32_t n, uint64_t deadline);

/* From kobj_signal, wait lock held: `o`'s signals are now `s`. */
void kqueue_signalled(struct kobj *o, uint32_t s);

/* Registrations that exist now, on every queue: the self-tests' leak check. */
uint32_t kqueue_reg_count(void);

#endif
