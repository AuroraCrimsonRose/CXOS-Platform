// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/memman/pmm.h */
/* Aurora Tejeda */
/*
 * Physical Memory Manager (PMM) - v1, bitmap allocator.
 *
 * Hands out physical RAM in 4 KB pages. One bit per page tracks
 * free (0) / used (1). Reads the BIOS E820 map to learn what RAM exists.
 *
 * The interface below is stable. On x86-64 it is the page database and buddy
 * allocator (memman/pagedb.c, pagedb.h); this bitmap stays the 32-bit
 * kernel's until roadmap step 6 deletes it.
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include "arch/paging.h"   /* paddr_t */

#define PMM_PAGE_SIZE 4096u

/* allocation flags (room to grow; e.g. huge pages later) */
#define PMM_FLAG_NONE 0x0

/* build the bitmap from the E820 map and mark reserved regions used. */
void pmm_init(void);

/* allocate / free a single 4 KB page. alloc returns its physical address,
   or 0 if out of memory. A physical address is a paddr_t, never a pointer:
   the kernel cannot dereference one until it is mapped. */
paddr_t pmm_alloc(void);
void    pmm_free(paddr_t page);

/* allocate / free N contiguous 4 KB pages (buddy allocator will improve
   the contiguous search later). */
paddr_t pmm_alloc_pages(size_t count);
void    pmm_free_pages(paddr_t first_page, size_t count);

/* N contiguous pages below 4 GB, for a device that addresses only 32 bits
   (the rig's OHCI controller is one; CX_KERNEL_PLAN section 9.3). Asking is
   part of every DMA allocation. On x86-32 all memory the PMM holds is below
   4 GB, so it is pmm_alloc_pages. */
paddr_t pmm_alloc_pages_low(size_t count);

/* 1 when the page is not the allocator's to give: handed out, or never
   given to it (the first MiB, the kernel image, the allocator's own
   records). 0 when it is free. For the self-tests (memman/dma.h). */
int pmm_page_in_use(paddr_t page);

/* stats (pages, not bytes) */
uint32_t pmm_total_pages(void);
uint32_t pmm_used_pages(void);
uint32_t pmm_free_count(void);

#endif