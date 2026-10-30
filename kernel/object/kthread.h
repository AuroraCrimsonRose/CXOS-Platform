// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kthread.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Thread objects (KOBJ_THREAD; docs/kernel/CX_KERNEL_PLAN.md section 7:
 * "user threads with thread handles (join is a wait)").
 *
 * What a thread handle names: not the scheduler's record (cpu/sched.h), which
 * is freed and its id reused once the thread is reaped, but this, which the
 * record and every handle hold a reference on. So a handle outlives the
 * thread, and a join after the end still finds how it ended: the way a
 * process handle outlives its process (Fuchsia's thread handles, XNU's thread
 * ports; not a tid, which Serenity and POSIX join through and which can
 * name a different thread by then).
 *
 * SIGNAL_THREAD_ENDED is set once, as the thread goes through thread_exit,
 * before its process's own SIGNAL_PROCESS_ENDED.
 */
#ifndef CXK_KTHREAD_H
#define CXK_KTHREAD_H

#include <stdint.h>
#include "object/kobj.h"

struct kthread {
    struct kobj hdr;     /* first; KOBJ_THREAD */
    int         tid;     /* the scheduler's id while it lives */
    uint32_t    ended;   /* THR_*; THR_RUNNING until kthread_end */
    int32_t     code;    /* THR_EXITED */
};

/* Register the type and its cache. Once, at boot. */
void kthread_sys_init(void);

/* A thread object for `tid`, with one reference, the caller's; 0 if there is
   no memory. */
struct kthread *kthread_create(int tid);

/* It has ended, `how` (THR_EXITED, THR_WITH_PROCESS), with `code`: noted and
   SIGNAL_THREAD_ENDED set. Only the first call counts. */
void kthread_end(struct kthread *k, uint32_t how, int32_t code);

#endif
