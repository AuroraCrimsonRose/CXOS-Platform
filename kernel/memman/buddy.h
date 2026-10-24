// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/buddy.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The page database and the buddy allocator over it
 * (docs/kernel/CX_KERNEL_PLAN.md section 9.3).
 *
 * One record per physical page, named by its page frame number (pfn): its
 * state, and while it heads a free block, the block's order and its links.
 * NT's PFN database (base/ntos/mm/pfnlist.c) and Haiku's vm_page are the
 * model. Free pages are kept in blocks of 2^order pages, each aligned to its
 * own size, one free list per order and zone; a freed block merges with its
 * buddy while the buddy is free and of the same order (Serenity
 * Kernel/Memory/PhysicalZone.cpp, Redox rmm/src/allocator/frame/buddy.rs).
 *
 * Two zones: below `low_end` (4 GB in the kernel), for devices that address
 * only 32 bits, and above it. A block never straddles them.
 *
 * Pure: no hardware, no locking, no panics. It refuses with an error and the
 * caller decides how fatal that is (memman/pagedb.c panics), so it is
 * host-tested (kernel/test/host/test_buddy.c).
 */

#ifndef MEMMAN_BUDDY_H
#define MEMMAN_BUDDY_H

#include <stdint.h>

#define BUDDY_MAX_ORDER   10u            /* 2^10 pages: 4 MiB */
#define BUDDY_ORDERS      (BUDDY_MAX_ORDER + 1u)
#define BUDDY_NONE        0xFFFFFFFFu    /* no page: an empty list, a failed allocation */

#define BUDDY_ZONE_LOW    0u             /* below low_end */
#define BUDDY_ZONE_HIGH   1u
#define BUDDY_ZONES       2u

/* page states */
#define PG_RESERVED  0u   /* not RAM, or RAM the allocator does not own */
#define PG_FREE      1u   /* in a free block */
#define PG_USED      2u   /* handed out */

#define PG_TAIL      0xFFu   /* the order of a free page that does not head its block */

/* errors */
#define BUDDY_E_RANGE     (-1)   /* past the database, or not aligned to the order */
#define BUDDY_E_NOT_USED  (-2)   /* a page in the range is not in use: a double free */

struct page {
    uint32_t next, prev;   /* free-list links (pfns) while this page heads a free block */
    uint32_t refs;         /* 1 while in use; room for sharing later */
    uint8_t  state;        /* PG_* */
    uint8_t  order;        /* heading a free block: its order; otherwise PG_TAIL */
    uint8_t  pad[2];
};

struct buddy {
    struct page *db;
    uint32_t     npages;                         /* pfn 0 .. npages-1 */
    uint32_t     low_end;                        /* first pfn of the high zone */
    uint32_t     head[BUDDY_ZONES][BUDDY_ORDERS];
    uint32_t     free_pages[BUDDY_ZONES];
    uint32_t     managed;                        /* pages ever made free */
};

/* Take `db` (npages records) with every page reserved and every list empty.
   `low_end` must be a multiple of 2^BUDDY_MAX_ORDER, so no block can straddle
   the zones; returns BUDDY_E_RANGE if it is not. */
int buddy_init(struct buddy *b, struct page *db, uint32_t npages, uint32_t low_end);

/* Give the allocator the reserved pages in [pfn, pfn+count). Pages already
   free or in use are left alone, so overlapping RAM ranges in a firmware map
   are harmless. Returns the number of pages added. */
uint32_t buddy_add(struct buddy *b, uint32_t pfn, uint32_t count);

/* A block of 2^order pages, aligned to its size, marked in use; BUDDY_NONE if
   there is none. From the high zone first, so the low zone stays for the
   devices that need it, unless `low_only`. */
uint32_t buddy_alloc(struct buddy *b, unsigned order, int low_only);

/* Free [pfn, pfn+count), every page of which must be in use: nothing is freed
   otherwise. The pages need not be one block; each merges as far as its
   buddies allow. 0, BUDDY_E_RANGE or BUDDY_E_NOT_USED. */
int buddy_free(struct buddy *b, uint32_t pfn, uint32_t count);

uint32_t buddy_free_count(const struct buddy *b);   /* both zones */

/* The smallest order whose block holds `count` pages, or BUDDY_ORDERS if none
   does. */
unsigned buddy_order_for(uint32_t count);

#endif
