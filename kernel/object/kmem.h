// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kmem.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Memory objects (KOBJ_MEM; docs/kernel/CX_KERNEL_PLAN.md section 9.4).
 *
 * A memory object holds pages; a mapping (memman/vmregion.h) is a view of
 * one. Its pages arrive on the first touch: the page fault asks kmem_page
 * for the page at an index, and a page nobody has touched is taken then and
 * zeroed (Serenity's AnonymousVMObject, Haiku's VMCache).
 *
 * STRICT COMMIT. Every page an object could ever hold is promised when it is
 * made: its size is charged to a job (cpu/job.h) and that many physical pages
 * are reserved (pmm_reserve), so a first touch always finds one. Running out
 * is the creator's E_NO_MEMORY, never a fault with nothing behind it. When
 * the object goes, its pages are freed and what was reserved and not taken is
 * given back, and so is the job's charge.
 *
 * The object owns its pages. A mapping holds a reference on the object and
 * maps its pages VM_BORROWED, so an address space torn down leaves them; the
 * object's own last reference frees them.
 *
 * The page array is under the object's lock, so a fault and the object's
 * other users (two mappings, 1.3e's other processes) see each page appear
 * once.
 */
#ifndef CXK_KMEM_H
#define CXK_KMEM_H

#include <stdint.h>
#include "object/kobj.h"
#include "sync/spinlock.h"
#include "arch/paging.h"

struct job;

struct kmem {
    struct kobj  hdr;        /* first; KOBJ_MEM */
    spinlock_t   lock;       /* pages and resident */
    uint32_t     npages;     /* its size; never 0 */
    uint32_t     resident;   /* pages taken so far */
    paddr_t     *pages;      /* npages entries, 0 until touched (kmalloc) */
    struct job  *job;        /* charged npages pages; a reference */
};

void kmem_sys_init(void);

/* A new object of `npages` (1 to MMAP_MAX_BYTES / PAGE_SIZE) zero pages,
   committed now and charged to `job`, with one reference for the caller.
   0 with *rc set: E_NO_MEMORY past the job's limit, past what memory can
   promise, or when the kernel has no room for its records. */
struct kmem *kmem_create(uint32_t npages, struct job *job, int *rc);

/* The page at `index` (< npages), taken and zeroed if this is its first
   touch. Never fails: the page was reserved when the object was made. */
paddr_t kmem_page(struct kmem *m, uint32_t index);

/* The page at `index` if it has been touched, or 0. */
paddr_t kmem_page_if_resident(struct kmem *m, uint32_t index);

/* Pages it holds now. */
uint32_t kmem_resident(struct kmem *m);

#endif
