// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_32/pagetable.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The 32-bit page-table format: two levels (directory -> table), 1024 entries
 * each, 4 KB pages. Only kernel/arch/x86_32/ includes this; generic code uses
 * arch/paging.h. The encoders are pure, so they are host-tested.
 *
 * Every space's directory:
 *   [0    .. 767 ]  user half        - private to the space (empty at create)
 *   [768  .. 1022]  kernel half      - SHARED: copied from the kernel's, so all
 *                                      spaces see the same kernel (0xC0000000+).
 *   [1023]          recursive slot   - points at THIS directory (per space).
 *
 * The kernel half is copied at create time. A change WITHIN an existing kernel
 * page table is seen by every space, since they share the table; a brand-new
 * kernel directory entry is pushed into every live space (addr_space.c).
 */

#ifndef ARCH_X86_32_PAGETABLE_H
#define ARCH_X86_32_PAGETABLE_H

#include "arch/paging.h"

/* entry bits (the same low bits in a directory and a table entry) */
#define PTE_PRESENT     0x001u
#define PTE_WRITE       0x002u
#define PTE_USER        0x004u   /* reachable from ring 3 */
#define PTE_WRITE_THRU  0x008u   /* PWT */
#define PTE_NO_CACHE    0x010u   /* PCD. Only needed in the table entry: for a
                                    4 KB page the directory's PCD governs caching
                                    of the page TABLE, not of the page it maps. */
#define PTE_ADDR_MASK   0xFFFFF000u

#define PT_ENTRIES      1024u

#define PD_KERNEL_LO    768u     /* KERNEL_VBASE >> 22 */
#define PD_KERNEL_HI    1022u
#define PD_RECURSIVE    1023u

_Static_assert(PD_KERNEL_LO == (KERNEL_VBASE >> 22), "the kernel half starts at KERNEL_VBASE");

/* The table entry for `phys` with what `vm` (VM_*) allows. Present always;
   ring 3 only when `user`. VM_EXEC sets nothing: without PAE there is no
   no-execute bit. Bits of `phys` below the page are dropped. */
uint32_t pte_encode(paddr_t phys, uint32_t vm, int user);

/* Fill a new space's directory: the user half empty, the kernel half copied
   from `kernel_pd`, the recursive slot pointing at `new_pd_phys`, kernel-only. */
void pd_init(uint32_t *new_pd, const uint32_t *kernel_pd, paddr_t new_pd_phys);

#ifndef CXK_HOST_TEST
/* paging.c calls this when it creates a kernel-half directory entry, so every
   live space gets it (addr_space.c). */
void addr_space_propagate_pde(uint32_t index, uint32_t value);

/* addr_space_init turns propagation on, once the kernel space is registered. */
void paging_propagate_on(void);
#endif

#endif
