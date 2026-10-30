// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/pagetable.c */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 page-table encoders (pagetable.h). Pure, so host-tested. */

#include "arch/x86_64/pagetable.h"

static uint64_t leaf_bits(uint32_t vm) {
    uint64_t e = PTE64_PRESENT;
    if (vm & VM_WRITE)     e |= PTE64_WRITE;
    if (!(vm & VM_EXEC))   e |= PTE64_NX;
    if (vm & VM_UNCACHED)  e |= PTE64_PCD | PTE64_PWT;
    if (vm & VM_BORROWED)  e |= PTE64_BORROWED;
    return e;
}

uint64_t pte64_encode(uint64_t phys, uint32_t vm, int user) {
    return (phys & PTE64_ADDR) | leaf_bits(vm) | (user ? PTE64_USER : 0);
}

uint64_t pde64_large(uint64_t phys, uint32_t vm) {
    return (phys & PTE64_ADDR_2M) | leaf_bits(vm) | PTE64_LARGE;
}

uint64_t pte64_table(uint64_t table_phys, int user) {
    return (table_phys & PTE64_ADDR) | PTE64_PRESENT | PTE64_WRITE | (user ? PTE64_USER : 0);
}

void pml4_init(uint64_t *new_pml4, const uint64_t *kernel_pml4) {
    for (unsigned i = 0; i < PML4_KERNEL_LO; i++) new_pml4[i] = 0;
    for (unsigned i = PML4_KERNEL_LO; i < PT64_ENTRIES; i++) new_pml4[i] = kernel_pml4[i];
}
