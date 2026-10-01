/* /CXLite/kernel/memman/pmm.h */
/* Aurora Tejeda */
/*
 * Physical Memory Manager (PMM) - v1, bitmap allocator.
 *
 * Hands out physical RAM in 4 KB pages. One bit per page tracks
 * free (0) / used (1). Reads the BIOS E820 map to learn what RAM exists.
 *
 * The interface below is stable: a future buddy allocator can replace the
 * bitmap internals without changing callers. pmm_alloc_pages() (contiguous
 * multi-page allocation) is the call the buddy allocator will most improve.
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PMM_PAGE_SIZE 4096u

/* allocation flags (room to grow; e.g. huge pages later) */
#define PMM_FLAG_NONE 0x0

/* build the bitmap from the E820 map and mark reserved regions used. */
void pmm_init(void);

/* allocate / free a single 4 KB page. alloc returns a physical address,
   or 0 if out of memory. */
void *pmm_alloc(void);
void  pmm_free(void *page);

/* allocate / free N contiguous 4 KB pages (buddy allocator will improve
   the contiguous search later). */
void *pmm_alloc_pages(size_t count);
void  pmm_free_pages(void *first_page, size_t count);

/* stats (pages, not bytes) */
uint32_t pmm_total_pages(void);
uint32_t pmm_used_pages(void);
uint32_t pmm_free_count(void);

#endif