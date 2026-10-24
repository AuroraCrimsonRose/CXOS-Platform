// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/pagetable.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The x86-64 page-table format: four levels (PML4 -> PDPT -> PD -> PT), 512
 * entries each, 4 KiB pages, and 2 MiB pages at the PD level for the direct
 * map. Only kernel/arch/x86_64/ includes this; generic code uses
 * arch/paging.h. The encoders are pure, so they are host-tested.
 *
 * Every space's PML4:
 *   [0   .. 255]  user half    - private to the space (empty at create)
 *   [256 .. 511]  kernel half  - SHARED: every entry is made at boot and
 *                                copied into each new space, so no kernel
 *                                entry is ever created later and nothing has
 *                                to be pushed into live spaces (x86-32's
 *                                propagation is gone; CX_KERNEL_PLAN 9.5).
 */

#ifndef ARCH_X86_64_PAGETABLE_H
#define ARCH_X86_64_PAGETABLE_H

#include <stdint.h>
#include "arch/paging.h"   /* VM_* */

#define PTE64_PRESENT   (1ull << 0)
#define PTE64_WRITE     (1ull << 1)
#define PTE64_USER      (1ull << 2)    /* reachable from ring 3 */
#define PTE64_PWT       (1ull << 3)
#define PTE64_PCD       (1ull << 4)    /* with PWT: uncached, whatever the MTRRs say */
#define PTE64_LARGE     (1ull << 7)    /* PS: at the PD level, a 2 MiB page */
#define PTE64_NX        (1ull << 63)   /* needs EFER.NXE, which the loader sets */
#define PTE64_ADDR      0x000FFFFFFFFFF000ull
#define PTE64_ADDR_2M   0x000FFFFFFFE00000ull

#define PT64_ENTRIES    512u
#define PML4_KERNEL_LO  256u

#define PML4_INDEX(v)   (((uint64_t)(v) >> 39) & 511u)
#define PDPT_INDEX(v)   (((uint64_t)(v) >> 30) & 511u)
#define PD_INDEX(v)     (((uint64_t)(v) >> 21) & 511u)
#define PT_INDEX(v)     (((uint64_t)(v) >> 12) & 511u)

#define PAGE_2M         0x200000ull

/* The 4 KiB entry for `phys` with what `vm` (VM_*) allows. Present always;
   no-execute unless VM_EXEC; uncached (PCD and PWT) with VM_UNCACHED; ring 3
   only when `user`. Bits of `phys` outside the address field are dropped. */
uint64_t pte64_encode(uint64_t phys, uint32_t vm, int user);

/* The 2 MiB entry for `phys`, as above, never reachable from ring 3. */
uint64_t pde64_large(uint64_t phys, uint32_t vm);

/* An entry that points at the next table down. Present and writable, never
   no-execute: the leaf decides those, since the CPU takes the strictest of
   every level. Ring 3 only when `user`. */
uint64_t pte64_table(uint64_t table_phys, int user);

/* Fill a new space's PML4: the user half empty, the kernel half copied from
   `kernel_pml4`. */
void pml4_init(uint64_t *new_pml4, const uint64_t *kernel_pml4);

#endif
