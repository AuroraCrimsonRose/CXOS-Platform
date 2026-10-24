// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_pagetable64.c */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 page-table encoders (arch/x86_64/pagetable.h): what a VM_*
   request becomes in an entry, and what a new space's PML4 holds. */
#include "khost.h"
#include "arch/x86_64/pagetable.h"

/* Every combination of the VM_* flags, kernel and user: exactly the bits asked
   for, present always, and no-execute unless VM_EXEC. */
KH_TEST(pagetable64_pte_is_what_was_asked) {
    for (uint32_t vm = 0; vm <= VM_ALL; vm++)
        for (int user = 0; user <= 1; user++) {
            uint64_t want = 0x0000123456789000ull | PTE64_PRESENT;
            if (vm & VM_WRITE)     want |= PTE64_WRITE;
            if (!(vm & VM_EXEC))   want |= PTE64_NX;
            if (vm & VM_UNCACHED)  want |= PTE64_PCD | PTE64_PWT;
            if (user)              want |= PTE64_USER;
            KH_CHECK_EQ(pte64_encode(0x0000123456789000ull, vm, user), want);
        }
}

/* A kernel mapping is never reachable from ring 3, whatever flags it carries. */
KH_TEST(pagetable64_kernel_entries_never_user) {
    for (uint32_t vm = 0; vm <= 0xFFu; vm++) {
        KH_CHECK_EQ(pte64_encode(0x1000u, vm, 0) & PTE64_USER, 0);
        KH_CHECK_EQ(pde64_large(0x200000u, vm) & PTE64_USER, 0);
    }
}

/* The address field holds only the address: low bits, and bits above the
   52-bit physical limit (where NX and the protection keys live), are not
   flags. */
KH_TEST(pagetable64_address_bits_stay_address) {
    KH_CHECK_EQ(pte64_encode(0x00400FFFull, VM_EXEC, 0), 0x00400000ull | PTE64_PRESENT);
    KH_CHECK_EQ(pte64_encode(0xFFFFFFFFFFFFFFFFull, VM_EXEC, 0),
                PTE64_ADDR | PTE64_PRESENT);
    KH_CHECK_EQ(pde64_large(0x003FFFFFull, VM_EXEC), 0x00200000ull | PTE64_PRESENT | PTE64_LARGE);
    KH_CHECK_EQ(pte64_table(0x00400FFFull, 0), 0x00400000ull | PTE64_PRESENT | PTE64_WRITE);
}

/* A large entry is a leaf with the same meaning, plus PS. */
KH_TEST(pagetable64_large_is_a_leaf) {
    for (uint32_t vm = 0; vm <= VM_ALL; vm++)
        KH_CHECK_EQ(pde64_large(0x40000000ull, vm),
                    pte64_encode(0x40000000ull, vm, 0) | PTE64_LARGE);
}

/* A table entry restricts nothing the leaf allows: writable and executable,
   since the CPU applies the strictest level. */
KH_TEST(pagetable64_table_entries_restrict_nothing) {
    KH_CHECK_EQ(pte64_table(0x5000u, 0), 0x5000ull | PTE64_PRESENT | PTE64_WRITE);
    KH_CHECK_EQ(pte64_table(0x5000u, 1), 0x5000ull | PTE64_PRESENT | PTE64_WRITE | PTE64_USER);
}

KH_TEST(pagetable64_new_pml4) {
    static uint64_t kpml4[PT64_ENTRIES], pml4[PT64_ENTRIES];
    for (uint32_t i = 0; i < PT64_ENTRIES; i++) {
        kpml4[i] = 0xA0000000ull + i * 0x1000ull + 3;
        pml4[i] = 0xDEADBEEFDEADBEEFull;
    }
    pml4_init(pml4, kpml4);
    for (uint32_t i = 0; i < PML4_KERNEL_LO; i++) KH_CHECK_EQ(pml4[i], 0);
    for (uint32_t i = PML4_KERNEL_LO; i < PT64_ENTRIES; i++) KH_CHECK_EQ(pml4[i], kpml4[i]);
}

/* The indices: KERNEL_VBASE's image window is PML4 511, PDPT 510. */
KH_TEST(pagetable64_indices) {
    KH_CHECK_EQ(PML4_INDEX(0xFFFFFFFF80000000ull), 511);
    KH_CHECK_EQ(PDPT_INDEX(0xFFFFFFFF80000000ull), 510);
    KH_CHECK_EQ(PML4_INDEX(0xFFFF800000000000ull), PML4_KERNEL_LO);
    KH_CHECK_EQ(PD_INDEX(0x40200000ull), 1);
    KH_CHECK_EQ(PT_INDEX(0x40201000ull), 1);
}
