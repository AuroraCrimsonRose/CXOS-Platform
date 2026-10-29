// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kobj.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Kernel objects: the one header every handle-able object starts with.
 * (docs/kernel/CX_KERNEL_PLAN.md section 2.1.)
 *
 * TWO COUNTS, as NT's object manager keeps them (WRK base/ntos/ob:
 * HandleCount and PointerCount), and as endpoints here already did by hand:
 *
 *   refs     every reference: each handle holds one, and so does any kernel
 *            code that keeps the pointer across a point where it can block.
 *            The object is destroyed when this reaches zero, and not before.
 *   handles  handles alone. When the last one closes, the type's
 *            `last_handle` hook runs: nobody can reach the object any more, but
 *            a blocked thread may still be standing on it, so it is closed for
 *            business (woken waiters, refused calls) without being freed.
 *
 * One counter cannot say "unreachable, but still in use", which is why there
 * are two.
 *
 * A TYPE MUST BE REGISTERED before an object of it can exist: kobj_init
 * refuses an unregistered type. Under the old handle table a release hook was
 * opt-in per type, and a type that never registered one looked exactly like a
 * type that worked (HANDOFF, "design traps"). Here the destroy hook is
 * required at registration, so that mistake cannot be made.
 *
 * Every count is atomic, so objects are already safe on several CPUs.
 */
#ifndef CXK_KOBJ_H
#define CXK_KOBJ_H

#include <stdint.h>
#include "sync/atomic.h"

enum kobj_type {
    KOBJ_NONE     = 0,
    KOBJ_ENDPOINT = 1,   /* IPC endpoint (cpu/ipc.c) */
    KOBJ_FILE     = 2,   /* open file (cpu/sysfile.c) */
    KOBJ_TEST     = 3,   /* the self-tests' own type; nothing else creates one */
    KOBJ_PROCESS  = 4,   /* process (cpu/proc.c) */
    KOBJ_JOB      = 5,   /* job (cpu/job.c) */
    KOBJ_TIMER    = 6,   /* timer (object/ktimer.c) */
    KOBJ_QUEUE    = 7,   /* queue (object/kqueue.c) */
    KOBJ_SEND     = 8,   /* send handle: an endpoint and a badge (cpu/ipc.c) */
    KOBJ_REPLY    = 9,   /* reply token: one call (cpu/ipc.c) */
    KOBJ_TYPE_MAX
};

struct kwaiter;   /* object/kwait.h */
struct kqreg;     /* object/kqueue.h */

struct kobj {
    uint16_t   type;      /* enum kobj_type */
    uint16_t   _pad;
    atomic_u32 refs;
    atomic_u32 handles;
    atomic_u32 signals;   /* signalled state (object/kwait.h); the type's own bits */
    uint32_t   koid;      /* kernel id: unique for the life of the system, never 0 */
    struct kwaiter *waiters;   /* threads in kobj_wait on it (object/kwait.h) */
    struct kqreg   *qregs;     /* queues' registrations on it (object/kqueue.h) */
};

struct kobj_ops {
    const char *name;
    /* Rights that mean something for this type; a handle can carry no others. */
    uint32_t    rights_valid;
    /* Signals it sets (object/kwait.h); 0: nothing to wait for. */
    uint32_t    signals_valid;
    /* Optional: the last handle has closed. The object may still be referenced. */
    void      (*last_handle)(struct kobj *o);
    /* Required: the last reference has gone. Free the object. */
    void      (*destroy)(struct kobj *o);
};

/* Register a type's operations, once, during boot. Returns 0, or -1 if `t` is
   out of range, `ops` has no destroy hook, or the type is already registered. */
int kobj_type_register(enum kobj_type t, const struct kobj_ops *ops);

/* The registered operations, or 0. */
const struct kobj_ops *kobj_type_ops(enum kobj_type t);

/* Start an object's life with one reference, held by the creator, and no
   handles. Returns 0, or -1 if the type is not registered (the object is then
   untouched and must not be used). */
int kobj_init(struct kobj *o, enum kobj_type t);

/* Take and drop a reference. Dropping the last one destroys the object. */
void kobj_ref(struct kobj *o);
void kobj_unref(struct kobj *o);

/* A handle was created for, or closed on, this object. Each handle holds a
   reference, so these adjust both counts. */
void kobj_handle_opened(struct kobj *o);
void kobj_handle_closed(struct kobj *o);

/* Objects of this type that exist now: the leak check every test ends with. */
uint32_t kobj_live(enum kobj_type t);

/* Called on a broken invariant (a count going below zero). The kernel panics;
   the host tests record it. Defined by whoever links kobj.c. */
void kobj_panic(const char *why);

#endif
