// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_pagetable.c */
/* Aurora Tejeda / CATX Systems */
/* The 32-bit page-table encoders (arch/x86_32/pagetable.h): what a VM_* request
   becomes in an entry, and what a new space's directory holds. */
#include "khost.h"
#include "arch/x86_32/pagetable.h"

/* Every combination of the VM_* flags, kernel and user: exactly the bits asked
   for, and present always. */
KH_TEST(pagetable_pte_is_what_was_asked) {
    for (uint32_t vm = 0; vm <= VM_ALL; vm++)
        for (int user = 0; user <= 1; user++) {
            uint32_t want = 0x12345000u | PTE_PRESENT;
            if (vm & VM_WRITE)    want |= PTE_WRITE;
            if (vm & VM_UNCACHED) want |= PTE_NO_CACHE;
            if (user)             want |= PTE_USER;
            KH_CHECK_EQ(pte_encode(0x12345000u, vm, user), want);
        }
}

/* A kernel mapping is never reachable from ring 3, whatever flags it carries:
   the USER bit comes only from `user`. */
KH_TEST(pagetable_kernel_entry_never_user) {
    for (uint32_t vm = 0; vm <= 0xFFu; vm++)
        KH_CHECK_EQ(pte_encode(0x1000u, vm, 0) & PTE_USER, 0);
}

/* The low bits of the address are not flags: an unaligned physical address
   cannot set USER, WRITE or anything else. */
KH_TEST(pagetable_address_bits_stay_address) {
    KH_CHECK_EQ(pte_encode(0x00400FFFu, 0, 0), 0x00400000u | PTE_PRESENT);
    KH_CHECK_EQ(pte_encode(0xFFFFFFFFu, 0, 0), 0xFFFFF000u | PTE_PRESENT);
}

/* Without PAE there is no no-execute bit, so VM_EXEC changes nothing - and in
   particular doesn't land in a bit that means something else. */
KH_TEST(pagetable_exec_sets_nothing) {
    KH_CHECK_EQ(pte_encode(0x2000u, VM_EXEC, 1), pte_encode(0x2000u, 0, 1));
    KH_CHECK_EQ(pte_encode(0x2000u, VM_EXEC | VM_WRITE, 0), pte_encode(0x2000u, VM_WRITE, 0));
}

KH_TEST(pagetable_new_directory) {
    static uint32_t kpd[PT_ENTRIES], pd[PT_ENTRIES];
    for (uint32_t i = 0; i < PT_ENTRIES; i++) { kpd[i] = 0xA0000000u + i * 0x1000u + 3; pd[i] = 0xDEADBEEFu; }
    pd_init(pd, kpd, 0x00777000u);
    int user_empty = 1, kernel_shared = 1;
    for (uint32_t i = 0; i < PD_KERNEL_LO; i++) if (pd[i]) user_empty = 0;
    for (uint32_t i = PD_KERNEL_LO; i <= PD_KERNEL_HI; i++) if (pd[i] != kpd[i]) kernel_shared = 0;
    KH_CHECK(user_empty);
    KH_CHECK(kernel_shared);
    /* The recursive slot names this directory, writable for the kernel and NOT
       for ring 3: with USER there, ring 3 could rewrite its own page tables
       through 0xFFC00000. */
    KH_CHECK_EQ(pd[PD_RECURSIVE], 0x00777000u | PTE_PRESENT | PTE_WRITE);
    KH_CHECK_EQ(PD_KERNEL_LO, KERNEL_VBASE >> 22);
}
