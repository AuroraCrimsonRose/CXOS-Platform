// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_32/pagetable.c */
/* Aurora Tejeda / CATX Systems */
/* The 32-bit page-table encoders. See pagetable.h. No hardware here. */

#include "arch/x86_32/pagetable.h"

uint32_t pte_encode(paddr_t phys, uint32_t vm, int user) {
    uint32_t e = (phys & PTE_ADDR_MASK) | PTE_PRESENT;
    if (vm & VM_WRITE)    e |= PTE_WRITE;
    if (vm & VM_UNCACHED) e |= PTE_NO_CACHE;
    if (user)             e |= PTE_USER;
    return e;
}

void pd_init(uint32_t *new_pd, const uint32_t *kernel_pd, paddr_t new_pd_phys) {
    for (uint32_t i = 0; i < PD_KERNEL_LO; i++)
        new_pd[i] = 0;                                         /* private user half */
    for (uint32_t i = PD_KERNEL_LO; i <= PD_KERNEL_HI; i++)
        new_pd[i] = kernel_pd[i];                              /* shared kernel half */
    /* Never PTE_USER: through this slot the directory and every table of the
       space appear at 0xFFC00000, and ring 3 would be editing its own page
       tables. */
    new_pd[PD_RECURSIVE] = (new_pd_phys & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE;
}
