// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/ocache.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Object caches: one per kernel object type (docs/kernel/CX_KERNEL_PLAN.md
 * section 9.2; Bonwick's slab allocator, Haiku slab/ObjectCache.cpp, XNU
 * kalloc_type in osfmk/kern/kalloc.c).
 *
 * A cache hands out objects of one size from slabs: runs of pages that hold
 * only that cache's objects. What that buys over the linked-list heap:
 *
 * TYPES NEVER SHARE A SLAB. A use-after-free can only ever be handed out again
 * as an object of the same type, so a stale pointer to an endpoint cannot be
 * made to alias an open file. (XNU's reason for kalloc_type.)
 *
 * THE RECORDS ARE NOT NEXT TO THE DATA. Each slab's record (owner, free bitmap,
 * list links) lives in a table indexed by the slab's address, not in a header
 * before the object, so overrunning an object cannot corrupt the allocator.
 *
 * MISTAKES ARE CAUGHT, NOT ABSORBED. Freeing a pointer that is not one of this
 * cache's objects - another cache's, an interior pointer, or one already free -
 * panics. A freed object is filled with OCACHE_POISON, and the fill is checked
 * when the object is handed out again, so a write after free panics at the
 * next allocation rather than corrupting whoever gets it. The fill is always
 * on, not a debug option: it is also what wipes one owner's data before the
 * next sees it.
 *
 * EMPTY SLABS GO BACK. The slab's pages are unmapped and freed when its last
 * object is, so a stale pointer into it faults instead of reading old data.
 *
 * The address space: slabs sit in a fixed kernel window of OCACHE_SLOTS spans,
 * each OCACHE_SPAN bytes, and a slab maps only the pages it uses, the rest of
 * its span staying unmapped as a guard. A slab holds at most
 * OCACHE_MAX_PER_SLAB objects (one 64-bit free map); an object larger than a
 * span is not this allocator's business.
 *
 * Every operation takes the cache's lock. Per-CPU magazines in front of it
 * come with SMP (plan section 9.2).
 */
#ifndef CXK_OCACHE_H
#define CXK_OCACHE_H

#include <stdint.h>
#include <stddef.h>
#include "sync/spinlock.h"
#include "arch/paging.h"   /* KVA_OCACHE */

#define OCACHE_SPAN          0x8000u     /* 32 KB of address space per slab */
#define OCACHE_PAGE          0x1000u
#define OCACHE_MAX_PER_SLAB  64
#define OCACHE_ALIGN         16u
#define OCACHE_POISON        0x6Bu

#ifndef OCACHE_SLOTS
#define OCACHE_SLOTS         2048        /* 64 MB of window */
#endif

struct ocache {
    const char *name;
    uint32_t    size;       /* object size, rounded up to OCACHE_ALIGN */
    uint32_t    pages;      /* pages mapped per slab */
    uint32_t    per_slab;   /* objects per slab */
    spinlock_t  lock;
    int32_t     partial;    /* slab lists, by slot; -1 = empty */
    int32_t     full;
    uint32_t    live;       /* objects handed out */
    uint32_t    slabs;      /* slabs mapped */
    struct ocache *all_next; /* every cache made, for ocache_each */
};

/* Set up a cache for objects of `size` bytes. Returns 0, or -1 if an object
   that size does not fit a span. */
int   ocache_init(struct ocache *c, const char *name, uint32_t size);

/* A zeroed object, or NULL if no page or no slot was to be had. */
void *ocache_alloc(struct ocache *c);

/* Give an object back. Panics on anything that is not a live object of `c`. */
void  ocache_free(struct ocache *c, void *obj);

uint32_t ocache_live(struct ocache *c);
uint32_t ocache_slabs(struct ocache *c);

/* Call fn on every cache made, in the order made, until it returns nonzero.
   Caches are never torn down, so the list only grows: a cache must outlive
   the kernel (static), which is why the host build keeps no list. */
void ocache_each(int (*fn)(struct ocache *c, void *ctx), void *ctx);
const char *ocache_name(const struct ocache *c);

/* Is `p` the start of a live object of `c`? */
int   ocache_owns(struct ocache *c, const void *p);

/* Is `p` inside the caches' window at all? And if so, the cache whose slab
   holds it, or NULL for a span no cache holds. For a caller that frees by
   address (kfree). */
int            ocache_in_window(const void *p);
struct ocache *ocache_owner(const void *p);

/* Call fn on every live object of `c`, under the cache's lock, until it
   returns nonzero; returns that value, or 0. fn must not allocate from or
   free to `c`. */
int   ocache_walk(struct ocache *c, int (*fn)(void *obj, void *ctx), void *ctx);

/*
 * Below this line, what the allocator needs from its host. The kernel's
 * versions are in ocache.c; the host tests supply their own, which is what
 * lets the allocator's logic run under the sanitizers.
 */

/* The window's first address; OCACHE_SPAN-aligned. */
uintptr_t ocache_window(void);
/* Back `pages` pages at `va` with zero-or-more-filled memory. 0, or -1 with
   nothing left mapped. */
int  ocache_map(uintptr_t va, uint32_t pages);
void ocache_unmap(uintptr_t va, uint32_t pages);
void ocache_panic(const char *why);

#ifndef CXK_HOST_TEST
/* The kernel's window: above the heap, which stops here. 64 MB. Placed with
   the rest of the kernel half in arch/<arch>/paging_defs.h. */
#define OCACHE_WINDOW_BASE   KVA_OCACHE

/* Reserve the window's page tables. Once, at boot, before any address space
   but the kernel's exists (as kstack_init). */
void ocache_sys_init(void);
#endif

#endif
