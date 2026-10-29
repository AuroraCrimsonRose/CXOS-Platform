// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kwait.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Signals and the single-object wait (docs/kernel/CX_KERNEL_PLAN.md section 5,
 * its "simple case"; SYS_OBJECT_WAIT).
 *
 * Every object has a signal word (kobj.signals). Its type says which bits it
 * sets (kobj_ops.signals_valid): a process sets SIGNAL_PROCESS_ENDED when its
 * last thread goes. A thread waits on one object for any of a set of signals,
 * with a timeout; setting a signal wakes every thread waiting on that object.
 *
 * The queue object of section 5 (register once, wait for many) builds on the
 * same word later; this is the one-handle wait it keeps.
 *
 * A waiter's record lives on its own kernel stack, linked into the object
 * while it waits. The caller of kobj_wait holds a reference on the object, so
 * neither can go away under the other. One lock covers every object's list:
 * a wait is rare and short, and one CPU runs today.
 */
#ifndef CXK_KWAIT_H
#define CXK_KWAIT_H

#include <stdint.h>
#include "object/kobj.h"

struct kwaiter {
    int             tid;
    struct kwaiter *next;
};

/* Set `bits` in o's signals and wake every thread waiting on it. Safe with
   interrupts off (the reaper calls it). */
void kobj_signal(struct kobj *o, uint32_t bits);

/* Wait until any of `want` is set in o's signals. `timeout_ms` 0 only looks;
   KWAIT_FOREVER has no limit. Returns 0 if one is set, or E_TIMED_OUT; either
   way *seen (if given) is the signal word as last read. The caller holds a
   reference on `o` and has checked `want` against the type. */
#define KWAIT_FOREVER 0xFFFFFFFFu
int kobj_wait(struct kobj *o, uint32_t want, uint32_t timeout_ms, uint32_t *seen);

/* Threads waiting on `o` now, for the self-tests. */
uint32_t kobj_waiters(struct kobj *o);

#endif
