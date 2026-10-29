// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/htab.h */
/* Aurora Tejeda / CATX Systems */
/*
 * A process's handle table. (docs/kernel/CX_KERNEL_PLAN.md section 2.1.)
 *
 * A handle names a kernel object (object/kobj.h) and carries the rights its
 * holder has over it. Four properties, each one a test in
 * kernel/test/host/test_htab.c:
 *
 * A CLOSED HANDLE'S NUMBER IS REFUSED. A handle value is a slot index plus the
 * slot's generation, and the generation moves on every close. A program that
 * keeps using a number after closing it gets E_BAD_HANDLE, not whatever object the
 * slot holds next. Under the old table a stale number silently named the next
 * occupant. The generation is 19 bits, so a number comes back only after its
 * slot has been reused 524,287 times; NT reuses handle values at once.
 *
 * 0 IS NEVER A HANDLE, and neither is any value below 4096 (the generation
 * is never 0; the ABI promises 256). A program that assumes small handles fails at once.
 *
 * RIGHTS ONLY SHRINK. A handle carries only rights its type defines
 * (kobj_ops.rights_valid) and the generic ones (HTAB_RIGHTS_GENERIC), and a
 * duplicate carries a subset of its source's. Only a handle holding
 * HTAB_RIGHT_DUPLICATE can be duplicated at all.
 *
 * EVERY HANDLE HAS A GRANT ID, unique for the life of the system (32 bits,
 * skipping 0), and records the grant it was derived from (0 for a handle made
 * with a new object). That chain is what revoking a grant, and asking "why
 * can this process do that?", will walk.
 *
 * THE TABLE GROWS. It starts empty, takes HTAB_FIRST_SLOTS on the first
 * handle and doubles when full, up to its limit (kernel.xkco max_handles, at
 * most HTAB_MAX_SLOTS). Growing never moves a handle to another slot, so no
 * value changes; a failed growth refuses the one handle and keeps the table.
 *
 * The table has its own lock. Object hooks (last_handle, destroy) never run
 * under it: a close takes the object out of the slot under the lock and drops
 * the handle after letting go, so a hook may wake threads or touch other
 * tables. A lookup returns the object with a reference taken, so it stays
 * alive however long the caller holds it, on any number of CPUs.
 */
#ifndef CXK_HTAB_H
#define CXK_HTAB_H

#include <stdint.h>
#include "object/kobj.h"
#include "sync/spinlock.h"

/* A handle value is (generation << HTAB_SLOT_BITS) | slot. */
#define HTAB_SLOT_BITS     12
#define HTAB_MAX_SLOTS     (1u << HTAB_SLOT_BITS)           /* 4096: the most any table may have */
#define HTAB_GEN_MAX       ((1u << (31 - HTAB_SLOT_BITS)) - 1)   /* values stay positive */
#define HTAB_FIRST_SLOTS   16      /* the array made for the first handle */
#define HTAB_LIMIT_DEFAULT 1024    /* a process's limit unless kernel.xkco says */
#define HTAB_LIMIT_MIN     16

/* The ABI's error values (cxk_abi.h). That header asserts 32-bit pointers, so the
   host tests cannot include it; htab.c checks these agree in the kernel build. */
#define HTAB_E_ACCESS_DENIED  (-7)
#define HTAB_E_INVALID_ARGS   (-3)
#define HTAB_E_NO_RESOURCES   (-9)
#define HTAB_E_BAD_HANDLE     (-5)

/* Generic rights, the same on every type (the ABI's HRIGHT_DUPLICATE, checked
   in htab.c in the kernel build). */
#define HTAB_RIGHT_DUPLICATE  0x80000000u
#define HTAB_RIGHT_WAIT       0x40000000u
#define HTAB_RIGHTS_GENERIC   (HTAB_RIGHT_DUPLICATE | HTAB_RIGHT_WAIT)

struct htab_entry {
    struct kobj *obj;      /* 0: the slot is free */
    uint32_t     rights;
    uint32_t     gen;      /* this occupant's generation, or the next one's; never 0 */
    uint32_t     grant;    /* this handle's grant id */
    uint32_t     parent;   /* the grant it was derived from; 0 if it made the object */
};

struct htab {
    spinlock_t         lock;
    struct htab_entry *e;      /* cap entries; 0 until the first handle */
    uint32_t           cap;
    uint32_t           max;    /* cap never passes it */
};

/* What a lookup can report besides the object. */
struct htab_info {
    uint32_t rights;
    uint32_t grant;
    uint32_t parent;
};

/* An empty table that may grow to `max` slots (at most HTAB_MAX_SLOTS).
   Allocates nothing. Every slot starts at generation 1. */
void htab_init(struct htab *t, uint32_t max);

/* Free the array. The table must have no handles (htab_close_all first). */
void htab_destroy(struct htab *t);

/* Give `o` a handle in `t` with `rights`, derived from `parent_grant` (0 for
   a handle on an object the caller just made). The handle holds its own
   reference; the caller's is untouched. Returns the handle value (> 0), or
   HTAB_E_INVALID_ARGS (no object, or its type is not registered), HTAB_E_ACCESS_DENIED (a right the type
   does not define) or HTAB_E_NO_RESOURCES (the table is at its limit, or could
   not grow). */
int32_t htab_install(struct htab *t, struct kobj *o, uint32_t rights, uint32_t parent_grant);

/* Look up `h`. It must name an object of `type` (KOBJ_NONE: any type) holding
   every right in `need`. Returns the object WITH A REFERENCE the caller must
   drop with kobj_unref, or 0. `info`, if given, is filled on success. */
struct kobj *htab_ref(struct htab *t, int32_t h, enum kobj_type type, uint32_t need,
                      struct htab_info *info);

/* A second handle in the same table on the same object, with `rights`, which
   must be a subset of the source's. Its parent grant is the source's grant.
   Returns the new value, or HTAB_E_BAD_HANDLE, HTAB_E_ACCESS_DENIED (the source lacks
   HTAB_RIGHT_DUPLICATE, or one of `rights`) or HTAB_E_NO_RESOURCES. */
int32_t htab_dup(struct htab *t, int32_t h, uint32_t rights);

/* Close `h`. Returns 0, or HTAB_E_BAD_HANDLE if it names nothing. */
int htab_close(struct htab *t, int32_t h);

/* Close every handle: process teardown. */
void htab_close_all(struct htab *t);

/* Open handles in the table. */
uint32_t htab_count(struct htab *t);

/* The array, freed with htab_mem_free: kmalloc in the kernel; the host tests
   bring their own, which can fail on demand. */
void *htab_mem_alloc(uint32_t bytes);
void  htab_mem_free(void *p);

#endif
