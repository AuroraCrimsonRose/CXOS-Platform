// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/pmm.c */
/* Aurora Tejeda / CATX Systems */
/* Physical Memory Manager - bitmap allocator (v5, higher-half aware). */

#include "arch/paging.h"   /* KERNEL_VBASE */
#include "pmm.h"
#include "align.h"
#include "bitmap.h"
#include "boot/cxbi.h"

/* --- higher-half access ---------------------------------------------------
 * The kernel runs at 0xC0000000+. The boot page tables map the first 4 MB of
 * physical RAM at 0xC0000000..0xC03FFFFF. To TOUCH a physical address P in that
 * window the kernel dereferences (P + KERNEL_VBASE). The bitmap, placed just
 * past the kernel near ~1 MB, lives inside the window, so this reaches it.
 * pmm_alloc returns PHYSICAL addresses.
 */
#define PHYS_TO_VIRT(p)  ((void *)((uint32_t)(p) + KERNEL_VBASE))

/* ignore RAM above ~3.75 GB (32-bit reach + device/ROM area near 4 GB). */
#define PMM_MAX_ADDR      0xF0000000u

extern uint8_t __kernel_end[];   /* end of kernel image (VIRTUAL), from linker */

static uint32_t *bitmap = 0;        /* VIRTUAL pointer into the mapped window */
static uint32_t  bitmap_phys = 0;
static uint32_t  total_pages = 0;
static uint32_t  used_pages = 0;
static uint32_t  bitmap_pages = 0;

/* thin wrappers over the pure bitmap lib that also maintain used_pages. */
static void mark_used(uint32_t page) {
    if (page >= total_pages) return;
    if (!bitmap_test(bitmap, page)) { bitmap_set(bitmap, page); used_pages++; }
}
static void mark_free(uint32_t page) {
    if (page >= total_pages) return;
    if (bitmap_test(bitmap, page)) { bitmap_clear(bitmap, page); used_pages--; }
}
static void mark_region_used(uint64_t start, uint64_t end) {
    uint32_t first = (uint32_t)(start / PMM_PAGE_SIZE);
    uint32_t last  = (uint32_t)((end + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    for (uint32_t p = first; p < last; p++) mark_used(p);
}

void pmm_init(void) {
    /* the loader's memory map (boot/cxbi.h) */
    uint32_t count = cxbi_mmap_count();

    /* 1. highest usable physical address, capped at the 32-bit limit */
    uint64_t highest = 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (e->type == CXBI_MEM_USABLE) {
            uint64_t top = e->base + e->length;
            if (top > highest) highest = top;
        }
    }
    if (highest > PMM_MAX_ADDR) highest = PMM_MAX_ADDR;
    total_pages = (uint32_t)(highest / PMM_PAGE_SIZE);

    /* 2. bitmap just past the kernel image, page-aligned. Physical address =
          __kernel_end (virtual) - KERNEL_VBASE; within the mapped 4 MB window. */
    uint32_t kend_phys = (uint32_t)__kernel_end - KERNEL_VBASE;
    bitmap_phys = align_up(kend_phys, PMM_PAGE_SIZE);
    bitmap = (uint32_t *)PHYS_TO_VIRT(bitmap_phys);

    uint32_t bitmap_bytes = (total_pages + 7) / 8;
    bitmap_pages = align_up(bitmap_bytes, PMM_PAGE_SIZE) / PMM_PAGE_SIZE;

    /* 3. everything used; then free the usable regions. */
    bitmap_fill(bitmap, total_pages, 1);
    used_pages = total_pages;

    for (uint32_t i = 0; i < count; i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (e->type == CXBI_MEM_USABLE) {
            uint64_t base_addr = e->base;
            uint64_t end_addr  = e->base + e->length;
            if (base_addr >= PMM_MAX_ADDR) continue;
            if (end_addr > PMM_MAX_ADDR) end_addr = PMM_MAX_ADDR;
            uint32_t first = (uint32_t)(base_addr / PMM_PAGE_SIZE);
            uint32_t last  = (uint32_t)(end_addr / PMM_PAGE_SIZE);
            for (uint32_t p = first; p < last; p++) mark_free(p);
        }
    }

    /* 4. re-reserve: low memory (<1 MB), the kernel image, the bitmap itself. */
    mark_region_used(0, 0x100000);
    mark_region_used(0x100000, kend_phys);
    mark_region_used(bitmap_phys, bitmap_phys + bitmap_pages * PMM_PAGE_SIZE);

    /* 5. driver DMA windows. The NIC drivers use a FIXED physical region
       (e1000 0x700000, RTL8169 0x730000) and hand it to the controller for
       bus-master DMA; AHCI and the USB controllers allocate theirs from here
       instead, so 0x500000-0x700000 is now unused. Those frames are above the kernel image,
       so without this they look free and pmm_alloc would hand them out - and the
       device would then DMA over whatever got them (heap, page tables, a process
       image) with no fault and no warning. Reserve the whole window. */
    mark_region_used(0x500000, 0x800000);
}

void *pmm_alloc(void) {
    uint32_t p = bitmap_first_clear(bitmap, total_pages);
    if (p == BITMAP_NONE) return 0;
    mark_used(p);
    return (void *)(p * PMM_PAGE_SIZE);   /* PHYSICAL address */
}

void pmm_free(void *page) {
    uint32_t p = (uint32_t)page / PMM_PAGE_SIZE;
    mark_free(p);
}

void *pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc();
    uint32_t start = bitmap_first_clear_run(bitmap, total_pages, (uint32_t)count);
    if (start == BITMAP_NONE) return 0;
    for (uint32_t q = start; q < start + count; q++) mark_used(q);
    return (void *)(start * PMM_PAGE_SIZE);
}

void pmm_free_pages(void *first_page, size_t count) {
    uint32_t start = (uint32_t)first_page / PMM_PAGE_SIZE;
    for (uint32_t p = start; p < start + count; p++) mark_free(p);
}

uint32_t pmm_total_pages(void) { return total_pages; }
uint32_t pmm_used_pages(void)  { return used_pages; }
uint32_t pmm_free_count(void)  { return total_pages - used_pages; }