// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/dma.c */
/* Aurora Tejeda / CATX Systems */
/* Device DMA memory, from the zone below 4 GB (dma.h). */

#include "memman/dma.h"
#include "pmm.h"
#include "arch/paging.h"
#include "sync/spinlock.h"

static struct dma_region regions[DMA_MAX_REGIONS];
static unsigned          nregions;
static spinlock_t        lock = SPINLOCK_INIT;

void *dma_alloc(uint32_t pages, uintptr_t vbase, uint32_t *phys, const char *name) {
    if (pages == 0) return 0;
    uint32_t f = spin_lock(&lock);
    int full = nregions == DMA_MAX_REGIONS;
    spin_unlock(&lock, f);
    if (full) return 0;

    paddr_t p = pmm_alloc_pages_low(pages);
    if (!p) return 0;
    /* the low zone's promise, kept as the rule every caller relies on */
    if ((uint64_t)p + (uint64_t)pages * 0x1000 > 0x100000000ull) {
        pmm_free_pages(p, pages);
        return 0;
    }
    uint32_t base = (uint32_t)p;
    for (uint32_t i = 0; i < pages; i++)
        paging_map_kernel(vbase + i * 0x1000, base + i * 0x1000, VM_WRITE);
    volatile uint8_t *v = (volatile uint8_t *)vbase;
    for (uint32_t i = 0; i < pages * 0x1000; i++) v[i] = 0;

    f = spin_lock(&lock);
    regions[nregions].phys  = base;
    regions[nregions].pages = pages;
    regions[nregions].name  = name;
    nregions++;
    spin_unlock(&lock, f);
    *phys = base;
    return (void *)vbase;
}

unsigned dma_region_count(void) { return nregions; }

const struct dma_region *dma_region_at(unsigned i) {
    return i < nregions ? &regions[i] : 0;
}
