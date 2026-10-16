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
 * keeps using a number after closing it gets E_BADF, not whatever object the
 * slot holds next. Under the old table a stale number silently named the next
 * occupant. The generation is 23 bits, so a number comes back only after its
 * slot has been reused 8,388,607 times; NT reuses handle values at once.
 *
 * 0 IS NEVER A HANDLE, and neither is any value below 256 (the generation is
 * never 0). A program that assumes small handles fails at once.
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

/* Slots per process. Fixed until processes become separate from threads
   (Phase 0 step 2), when tables grow on demand. At most 1 << HTAB_SLOT_BITS. */
#define HTAB_SLOTS      16
#define HTAB_SLOT_BITS  8
#define HTAB_GEN_MAX    0x7FFFFFu     /* 31 - HTAB_SLOT_BITS bits: values stay positive */

/* The ABI's error values (cxk_abi.h). That header asserts 32-bit pointers, so the
   host tests cannot include it; htab.c checks these agree in the kernel build. */
#define HTAB_E_PERM   (-1)
#define HTAB_E_INVAL  (-2)
#define HTAB_E_NOMEM  (-5)
#define HTAB_E_BADF   (-6)

/* Generic rights, the same on every type (the ABI's HRIGHT_DUPLICATE, checked
   in htab.c in the kernel build). */
#define HTAB_RIGHT_DUPLICATE  0x80000000u
#define HTAB_RIGHTS_GENERIC   HTAB_RIGHT_DUPLICATE

struct htab_entry {
    struct kobj *obj;      /* 0: the slot is free */
    uint32_t     rights;
    uint32_t     gen;      /* this occupant's generation, or the next one's; never 0 */
    uint32_t     grant;    /* this handle's grant id */
    uint32_t     parent;   /* the grant it was derived from; 0 if it made the object */
};

struct htab {
    spinlock_t        lock;
    struct htab_entry e[HTAB_SLOTS];
};

/* What a lookup can report besides the object. */
struct htab_info {
    uint32_t rights;
    uint32_t grant;
    uint32_t parent;
};

/* An empty table. Every slot starts at generation 1. */
void htab_init(struct htab *t);

/* Give `o` a handle in `t` with `rights`, derived from `parent_grant` (0 for
   a handle on an object the caller just made). The handle holds its own
   reference; the caller's is untouched. Returns the handle value (> 0), or
   HTAB_E_INVAL (no object, or its type is not registered), HTAB_E_PERM (a right the type
   does not define) or HTAB_E_NOMEM (the table is full). */
int32_t htab_install(struct htab *t, struct kobj *o, uint32_t rights, uint32_t parent_grant);

/* Look up `h`. It must name an object of `type` (KOBJ_NONE: any type) holding
   every right in `need`. Returns the object WITH A REFERENCE the caller must
   drop with kobj_unref, or 0. `info`, if given, is filled on success. */
struct kobj *htab_ref(struct htab *t, int32_t h, enum kobj_type type, uint32_t need,
                      struct htab_info *info);

/* A second handle in the same table on the same object, with `rights`, which
   must be a subset of the source's. Its parent grant is the source's grant.
   Returns the new value, or HTAB_E_BADF, HTAB_E_PERM (the source lacks
   HTAB_RIGHT_DUPLICATE, or one of `rights`) or HTAB_E_NOMEM. */
int32_t htab_dup(struct htab *t, int32_t h, uint32_t rights);

/* Close `h`. Returns 0, or HTAB_E_BADF if it names nothing. */
int htab_close(struct htab *t, int32_t h);

/* Close every handle: process teardown. */
void htab_close_all(struct htab *t);

/* Open handles in the table. */
uint32_t htab_count(struct htab *t);

#endif
