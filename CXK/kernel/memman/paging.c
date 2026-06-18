/* /CXK/kernel/memman/paging.c */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * x86 32-bit paging - v5, RECURSIVE page directory.
 *
 * Unlike v4 (identity-mapped, enabled paging itself), v5 inherits paging ALREADY
 * ON from kernel.asm's higher-half bootstrap. paging_init does NOT re-enable
 * paging; it ADOPTS the existing boot page directory and installs the recursive
 * mapping so the kernel can edit page tables it can't otherwise reach.
 *
 * Recursive trick: PDE[1023] points at the page directory itself. Then:
 *   - the page directory is visible at virtual 0xFFFFF000
 *   - the page table for PD index i is visible at 0xFFC00000 + (i << 12)
 *   - the PTE mapping virtual V is at  0xFFC00000 + (PD_INDEX(V)<<12) + PT_INDEX(V)*4
 * This costs the top 4 MB of virtual space (0xFFC00000+), which the kernel
 * doesn't need. (Address math verified against known cases before writing this.)
 */

#include "paging.h"
#include "pmm.h"

#define KERNEL_VBASE   0xC0000000u
#define RECURSIVE_SLOT 1023

/* the boot page directory built by kernel.asm (a kernel symbol => virtual addr).
   Its PHYSICAL address is (virtual - KERNEL_VBASE). */
extern uint32_t boot_page_dir[];

#define PD_INDEX(v)  (((v) >> 22) & 0x3FF)
#define PT_INDEX(v)  (((v) >> 12) & 0x3FF)

/* recursive virtual addresses */
#define PD_VIRT      ((volatile uint32_t *)0xFFFFF000u)
static inline volatile uint32_t *pt_virt(uint32_t v) {
    return (volatile uint32_t *)(0xFFC00000u + (PD_INDEX(v) << 12));
}

static inline uint32_t virt_to_phys_kernel(void *p) {
    return (uint32_t)p - KERNEL_VBASE;
}

static inline void invlpg(uint32_t v) {
    __asm__ volatile ("invlpg (%0)" : : "r"(v) : "memory");
}

void paging_init(void) {
    /* install the recursive entry into the existing boot page directory:
       PDE[1023] -> the PD's own physical address, present + writable. */
    uint32_t pd_phys = virt_to_phys_kernel(boot_page_dir);
    boot_page_dir[RECURSIVE_SLOT] = pd_phys | PAGE_PRESENT | PAGE_WRITE;

    /* reload CR3 to flush the TLB so the recursive mapping takes effect. */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

void paging_map(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pdi = PD_INDEX(virt);

    /* ensure a page table exists for this region. PD is at PD_VIRT via recursion. */
    if (!(PD_VIRT[pdi] & PAGE_PRESENT)) {
        /* allocate a fresh page table frame (physical) from the PMM */
        uint32_t new_pt = (uint32_t)pmm_alloc();
        if (!new_pt) return;   /* out of memory */
        /* install it into the PD: present + writable (+ user if requested) */
        PD_VIRT[pdi] = (new_pt & ~0xFFFu) | PAGE_PRESENT | PAGE_WRITE
                     | (flags & PAGE_USER);
        /* the new table is now reachable at pt_virt(virt); flush + zero it. */
        invlpg((uint32_t)pt_virt(virt));
        volatile uint32_t *t = pt_virt(virt);
        for (int i = 0; i < PAGE_ENTRIES; i++) t[i] = 0;
    }

    /* write the PTE through the recursive page-table window */
    volatile uint32_t *table = pt_virt(virt);
    table[PT_INDEX(virt)] = (phys & ~0xFFFu) | (flags & 0xFFF) | PAGE_PRESENT;

    /* CPU ANDs privilege bits across BOTH levels: make sure the PDE also grants
       USER/WRITE if this mapping needs them (the PDE may have been created
       supervisor-only by an earlier mapping). */
    if (flags & PAGE_USER)  PD_VIRT[pdi] |= PAGE_USER;
    if (flags & PAGE_WRITE) PD_VIRT[pdi] |= PAGE_WRITE;

    invlpg(virt);
}

void paging_unmap(uint32_t virt) {
    uint32_t pdi = PD_INDEX(virt);
    if (!(PD_VIRT[pdi] & PAGE_PRESENT)) return;   /* no table => nothing mapped */
    volatile uint32_t *table = pt_virt(virt);
    table[PT_INDEX(virt)] = 0;
    invlpg(virt);
}

uint32_t paging_get_phys(uint32_t virt) {
    uint32_t pdi = PD_INDEX(virt);
    if (!(PD_VIRT[pdi] & PAGE_PRESENT)) return 0;
    volatile uint32_t *table = pt_virt(virt);
    uint32_t entry = table[PT_INDEX(virt)];
    if (!(entry & PAGE_PRESENT)) return 0;
    return (entry & ~0xFFFu) | (virt & 0xFFFu);
}