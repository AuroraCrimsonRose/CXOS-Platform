/* /CXLite/kernel/memman/pmm.c */
/* Aurora Tejeda */
/* Physical Memory Manager - v1 bitmap allocator. */

#include "pmm.h"

/* E820 map left by the bootloader (see stage2.asm). Raw buffer = all regions. */
#define E820_BUFFER       0x1000
#define E820_ENTRY_COUNT  0x1600
#define E820_TYPE_USABLE  1

/* 32-bit addressing ceiling. We can only address ~4 GB with 32-bit pointers,
   so the PMM ignores any RAM above this (e.g. on a 16 GB machine). The future
   x86_64 branch will use the rest. We cap a little under 4 GB to stay clear of
   the top-of-32-bit edge and the memory-mapped device/ROM area near 4 GB.
   Cap = 0xF0000000 (3.75 GB) of manageable physical pages. */
#define PMM_MAX_ADDR      0xF0000000u

/* Where to place the allocation bitmap: in EXTENDED memory (above 1 MB), not
   in the cramped <1 MB low memory (which holds the stack at 0x90000, VGA at
   0xA0000, BIOS, etc.). A 4 GB machine needs a ~128 KB bitmap, which does NOT
   fit in low memory but fits easily up here. Placed at 2 MB, well above the
   kernel (loaded at 0x10000) and the 1 MB line. */
#define PMM_BITMAP_ADDR   0x200000u    /* 2 MB */

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t flags;
} __attribute__((packed));

/* end of the kernel image, from the linker script */
extern uint8_t __bss_end[];

/* --- bitmap state --- */
static uint32_t *bitmap = 0;        /* one bit per page: 1 = used, 0 = free */
static uint32_t  total_pages = 0;   /* pages of physical RAM we track */
static uint32_t  used_pages = 0;
static uint32_t  bitmap_pages = 0;  /* how many pages the bitmap itself occupies */

/* --- bitmap helpers --- */
static void bm_set(uint32_t page) {
    bitmap[page >> 5] |= (1u << (page & 31));
}
static void bm_clear(uint32_t page) {
    bitmap[page >> 5] &= ~(1u << (page & 31));
}
static int bm_test(uint32_t page) {
    return (bitmap[page >> 5] >> (page & 31)) & 1u;
}

/* mark a single page used / free, updating the count */
static void mark_used(uint32_t page) {
    if (page >= total_pages) return;
    if (!bm_test(page)) { bm_set(page); used_pages++; }
}
static void mark_free(uint32_t page) {
    if (page >= total_pages) return;
    if (bm_test(page)) { bm_clear(page); used_pages--; }
}

/* mark a physical address range [start, end) used (rounded to pages) */
static void mark_region_used(uint64_t start, uint64_t end) {
    uint32_t first = (uint32_t)(start / PMM_PAGE_SIZE);
    uint32_t last  = (uint32_t)((end + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    for (uint32_t p = first; p < last; p++) mark_used(p);
}

void pmm_init(void) {
    uint16_t count = *(volatile uint16_t *)E820_ENTRY_COUNT;
    struct e820_entry *e = (struct e820_entry *)E820_BUFFER;

    /* 1. find highest usable physical address, but CAP at the 32-bit limit -
       we can't address RAM above ~4 GB with 32-bit pointers, so we ignore it
       (a 16 GB machine still only gets ~3.75 GB managed here). */
    uint64_t highest = 0;
    for (int i = 0; i < count; i++) {
        if (e[i].type == E820_TYPE_USABLE) {
            uint64_t top = e[i].base + e[i].length;
            if (top > highest) highest = top;
        }
    }
    if (highest > PMM_MAX_ADDR) highest = PMM_MAX_ADDR;

    total_pages = (uint32_t)(highest / PMM_PAGE_SIZE);

    /* 2. place the bitmap in EXTENDED memory (2 MB), not low memory. A machine
       near the 4 GB cap needs a ~128 KB bitmap, which would overflow the
       <1 MB region (smashing the stack at 0x90000). 2 MB has room. */
    bitmap = (uint32_t *)PMM_BITMAP_ADDR;

    uint32_t bitmap_bytes = (total_pages + 7) / 8;
    bitmap_pages = (bitmap_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    /* 3. start with EVERYTHING used, then free the usable regions.
       (safer default: anything we don't explicitly free stays reserved) */
    for (uint32_t i = 0; i < (total_pages + 31) / 32; i++) bitmap[i] = 0xFFFFFFFF;
    used_pages = total_pages;

    /* 4. free pages that E820 says are usable (clamped to the cap) */
    for (int i = 0; i < count; i++) {
        if (e[i].type == E820_TYPE_USABLE) {
            uint64_t base_addr = e[i].base;
            uint64_t end_addr  = e[i].base + e[i].length;
            if (base_addr >= PMM_MAX_ADDR) continue;        /* entirely above cap */
            if (end_addr > PMM_MAX_ADDR) end_addr = PMM_MAX_ADDR;
            uint32_t first = (uint32_t)(base_addr / PMM_PAGE_SIZE);
            uint32_t last  = (uint32_t)(end_addr / PMM_PAGE_SIZE);
            for (uint32_t p = first; p < last; p++) mark_free(p);
        }
    }

    /* 5. re-reserve the things that must never be handed out:
          - low memory below 1 MB (BIOS, bootloader buffers, VGA, E820 data)
          - the kernel image itself (0x10000 .. __bss_end)
          - the bitmap's own pages (now at 2 MB) */
    mark_region_used(0, 0x100000);                                  /* < 1 MB */
    mark_region_used(0x10000, (uint32_t)__bss_end);                 /* kernel */
    mark_region_used((uint32_t)bitmap,
                     (uint32_t)bitmap + bitmap_pages * PMM_PAGE_SIZE); /* bitmap */
}

void *pmm_alloc(void) {
    for (uint32_t p = 0; p < total_pages; p++) {
        if (!bm_test(p)) {
            mark_used(p);
            return (void *)(p * PMM_PAGE_SIZE);
        }
    }
    return 0;   /* out of memory */
}

void pmm_free(void *page) {
    uint32_t p = (uint32_t)page / PMM_PAGE_SIZE;
    mark_free(p);
}

void *pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc();

    /* scan for `count` consecutive free pages (the buddy allocator will
       replace this linear search with something far better). */
    uint32_t run = 0, start = 0;
    for (uint32_t p = 0; p < total_pages; p++) {
        if (!bm_test(p)) {
            if (run == 0) start = p;
            if (++run == count) {
                for (uint32_t q = start; q < start + count; q++) mark_used(q);
                return (void *)(start * PMM_PAGE_SIZE);
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

void pmm_free_pages(void *first_page, size_t count) {
    uint32_t start = (uint32_t)first_page / PMM_PAGE_SIZE;
    for (uint32_t p = start; p < start + count; p++) mark_free(p);
}

uint32_t pmm_total_pages(void) { return total_pages; }
uint32_t pmm_used_pages(void)  { return used_pages; }
uint32_t pmm_free_count(void)  { return total_pages - used_pages; }