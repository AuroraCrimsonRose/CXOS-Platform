// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/pagedb.c */
/* Aurora Tejeda / CATX Systems */
/*
 * pmm.h on x86-64: one record per physical page (the page database), the
 * buddy allocator over it (memman/buddy.c), and a zone below 4 GB
 * (docs/kernel/CX_KERNEL_PLAN.md section 9.3). pagedb.h has the rest.
 *
 * The database covers every page up to the top of usable RAM, holes
 * included, at 16 bytes a page (0.4%). It is placed in the highest usable
 * range that holds it, so the low zone stays for devices, and reached
 * through the direct map. The allocator owns usable RAM except: the first
 * MiB (firmware, the loader, real-mode leftovers), the kernel image, the
 * pages paging_init took for its tables, and the database itself. Anything
 * any other map entry covers, even where a usable entry overlaps it, is
 * left alone: firmware maps can overlap, and the cautious reading wins.
 */

#include "pmm.h"
#include "memman/buddy.h"
#include "memman/pagedb.h"
#include "boot/cxbi.h"
#include "sync/spinlock.h"
#include "panic.h"

#define LOW_ZONE_END_PFN  0x100000u              /* 4 GB */
#define LOW_MEMORY_END    0x100000ull            /* the first MiB */
#define PFN_LIMIT         0xFFFFF000u            /* pfns are 32-bit in the database */

extern uint8_t __kernel_end[];                   /* the linker's, virtual */

static struct buddy b;
static spinlock_t   lock = SPINLOCK_INIT;
static int          ready;

static uint64_t boot_start, boot_next, boot_end;   /* pmm_boot_alloc's range */
static uint64_t db_start, db_bytes;

static uint64_t up4k(uint64_t x)   { return (x + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1); }
static uint64_t down4k(uint64_t x) { return x & ~(uint64_t)(PAGE_SIZE - 1); }

static uint64_t image_end(void) {
    return up4k((uint64_t)(uintptr_t)__kernel_end - KERNEL_VBASE);
}

/* ---- before the allocator: pages for paging_init ---------------------------- */

paddr_t pmm_boot_alloc(paddr_t below) {
    if (ready) kpanic("pmm_boot_alloc after pmm_init");
    if (!boot_end) {
        /* the largest usable stretch above the image and below `below` */
        uint64_t kend = image_end();
        for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
            const struct cxk_mmap_entry *e = cxbi_mmap(i);
            if (e->type != CXBI_MEM_USABLE) continue;
            uint64_t lo = up4k(e->base), hi = down4k(e->base + e->length);
            if (lo < kend) lo = kend;
            if (hi > below) hi = down4k(below);
            if (lo < hi && hi - lo > boot_end - boot_start) { boot_start = lo; boot_end = hi; }
        }
        boot_next = boot_start;
    }
    if (boot_next + PAGE_SIZE > boot_end) return 0;
    boot_next += PAGE_SIZE;
    return boot_next - PAGE_SIZE;
}

/* ---- what the allocator must not own ------------------------------------------ */

/* The first range [lo, hi) overlaps that is not the allocator's, rounded out
   to whole pages; 0 if none. */
static int overlap(uint64_t lo, uint64_t hi, uint64_t *rlo, uint64_t *rhi) {
    const uint64_t fixed[][2] = {
        { 0,          LOW_MEMORY_END },
        { 0x100000,   image_end() },
        { boot_start, boot_next },
        { db_start,   db_start + db_bytes },
    };
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        if (fixed[i][0] < fixed[i][1] && fixed[i][0] < hi && lo < fixed[i][1]) {
            *rlo = fixed[i][0];
            *rhi = fixed[i][1];
            return 1;
        }
    }
    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (e->type == CXBI_MEM_USABLE || e->length == 0) continue;
        uint64_t elo = down4k(e->base), ehi = up4k(e->base + e->length);
        if (elo < hi && lo < ehi) { *rlo = elo; *rhi = ehi; return 1; }
    }
    return 0;
}

/* Give the allocator [lo, hi), less everything overlap() names. */
static void add_usable(uint64_t lo, uint64_t hi) {
    lo = up4k(lo);
    hi = down4k(hi);
    if (hi > (uint64_t)b.npages * PAGE_SIZE) hi = (uint64_t)b.npages * PAGE_SIZE;
    if (lo >= hi) return;
    uint64_t rlo, rhi;
    if (overlap(lo, hi, &rlo, &rhi)) {
        add_usable(lo, rlo);
        add_usable(rhi, hi);
        return;
    }
    buddy_add(&b, (uint32_t)(lo / PAGE_SIZE), (uint32_t)((hi - lo) / PAGE_SIZE));
}

/* The database's place: the top of the highest usable range that holds it
   clear of everything else. */
static void place_database(uint64_t bytes) {
    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (e->type != CXBI_MEM_USABLE) continue;
        uint64_t lo = up4k(e->base), hi = down4k(e->base + e->length);
        if (hi > DIRECT_MAP_SIZE) hi = DIRECT_MAP_SIZE;
        if (hi < lo + bytes || hi - bytes <= db_start) continue;
        uint64_t rlo, rhi;
        if (overlap(hi - bytes, hi, &rlo, &rhi)) continue;
        db_start = hi - bytes;
    }
    if (!db_start) kpanic("pmm: no usable range holds the page database");
    db_bytes = bytes;
}

void pmm_init(void) {
    uint64_t top = 0;
    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (e->type == CXBI_MEM_USABLE && e->base + e->length > top) top = e->base + e->length;
    }
    if (top > DIRECT_MAP_SIZE) top = DIRECT_MAP_SIZE;
    uint64_t npages = down4k(top) / PAGE_SIZE;
    if (npages > PFN_LIMIT) npages = PFN_LIMIT;

    place_database(up4k(npages * sizeof(struct page)));
    struct page *db = (struct page *)(uintptr_t)(DIRECT_MAP_BASE + db_start);
    if (buddy_init(&b, db, (uint32_t)npages, LOW_ZONE_END_PFN) != 0)
        kpanic("pmm: the low zone does not end on a block boundary");

    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (e->type == CXBI_MEM_USABLE) add_usable(e->base, e->base + e->length);
    }
    ready = 1;
}

/* ---- pmm.h ------------------------------------------------------------------- */

static paddr_t alloc_run(size_t count, int low_only) {
    unsigned order = buddy_order_for(count > 0xFFFFFFFFu ? 0 : (uint32_t)count);
    if (order >= BUDDY_ORDERS) return 0;
    uint32_t f = spin_lock(&lock);
    uint32_t pfn = buddy_alloc(&b, order, low_only);
    /* a run that is not a power of two gives the rest of its block back */
    if (pfn != BUDDY_NONE && count < (1u << order))
        buddy_free(&b, pfn + (uint32_t)count, (1u << order) - (uint32_t)count);
    spin_unlock(&lock, f);
    return pfn == BUDDY_NONE ? 0 : (paddr_t)pfn * PAGE_SIZE;
}

paddr_t pmm_alloc(void)                      { return alloc_run(1, 0); }
paddr_t pmm_alloc_pages(size_t count)        { return alloc_run(count, 0); }
paddr_t pmm_alloc_pages_low(size_t count)    { return alloc_run(count, 1); }

int pmm_page_in_use(paddr_t page) {
    uint64_t pfn = page / PAGE_SIZE;
    if (pfn >= b.npages) return 1;
    uint32_t f = spin_lock(&lock);
    int used = b.db[pfn].state != PG_FREE;
    spin_unlock(&lock, f);
    return used;
}

int pmm_free_checked(paddr_t first, size_t count) {
    if ((first & (PAGE_SIZE - 1)) || count > 0xFFFFFFFFu) return -1;
    uint64_t pfn = first / PAGE_SIZE;
    if (pfn > 0xFFFFFFFFu) return -1;
    uint32_t f = spin_lock(&lock);
    int rc = buddy_free(&b, (uint32_t)pfn, (uint32_t)count);
    spin_unlock(&lock, f);
    return rc == 0 ? 0 : -1;
}

void pmm_free(paddr_t page) {
    if (pmm_free_checked(page, 1) != 0) kpanic("pmm_free: the page is not in use");
}

void pmm_free_pages(paddr_t first_page, size_t count) {
    if (pmm_free_checked(first_page, count) != 0)
        kpanic("pmm_free_pages: a page in the run is not in use");
}

uint32_t pmm_total_pages(void) { return b.managed; }
uint32_t pmm_free_count(void)  { return buddy_free_count(&b); }
uint32_t pmm_used_pages(void)  { return b.managed - buddy_free_count(&b); }

void pagedb_database(paddr_t *start, uint64_t *bytes)    { *start = db_start; *bytes = db_bytes; }
void pagedb_boot_tables(paddr_t *start, uint64_t *bytes) { *start = boot_start; *bytes = boot_next - boot_start; }
