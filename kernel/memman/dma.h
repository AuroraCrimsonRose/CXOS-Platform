// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/dma.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Memory a device reaches by bus-master DMA.
 *
 * Every driver that hands a controller physical addresses takes them from
 * here: the NICs' rings and buffers, AHCI's command structures and bounce
 * buffer, each USB host controller's schedule. The pages come from the
 * allocator's zone below 4 GB, because every one of those drivers programs
 * 32-bit addresses (the rig's OHCI has nothing wider; the others leave the
 * high halves at 0), and asking for that zone is part of the allocation
 * (docs/kernel/CX_KERNEL_PLAN.md section 9.3).
 *
 * Before roadmap step 5f2 the NICs used fixed regions (e1000 0x700000,
 * RTL8169 0x730000) and pmm.c reserved 0x500000-0x800000 so nothing else
 * got them. The page database on x86-64 never had that reserve, so a fixed
 * region there was RAM the allocator also gave away: the device would DMA
 * over a page table or a heap block with no fault. Allocated regions make
 * the reserve unnecessary, and the allocator's own records say who owns
 * every page.
 */

#ifndef CXK_MEMMAN_DMA_H
#define CXK_MEMMAN_DMA_H

#include <stdint.h>

/* `pages` physically contiguous pages below 4 GB, zeroed, mapped cacheable
   (x86 snoops bus-master DMA) at `vbase` in the kernel half, so the CPU
   reaches them whatever CR3 holds. The device is given *phys; the CPU uses
   the returned alias. `name` is the driver's, for the self-tests. Returns 0
   when the low zone has no run that long, or the ledger below is full. */
void *dma_alloc(uint32_t pages, uintptr_t vbase, uint32_t *phys, const char *name);

/* The ledger of every region dma_alloc gave out, for the self-tests: each
   must lie below 4 GB and every page of it be in use. Never freed; no driver
   gives its DMA memory back. */
#define DMA_MAX_REGIONS 32   /* 14 USB host controllers at most, AHCI's two, the NICs */
struct dma_region {
    uint32_t    phys;
    uint32_t    pages;
    const char *name;
};
unsigned dma_region_count(void);
const struct dma_region *dma_region_at(unsigned i);

#endif
