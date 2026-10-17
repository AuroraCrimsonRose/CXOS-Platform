// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/paging.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Paging, as generic code sees it (docs/kernel/CX_KERNEL_PLAN.md section 2.2).
 * One interface, one implementation per architecture, as Haiku does
 * (src/system/kernel/arch/x86/paging/{32bit,pae,64bit}): kernel/arch/x86_32/
 * paging.c and addr_space.c today.
 *
 * A mapping is described by what it allows (VM_*), never by the bits of a page
 * table entry; the architecture encodes it. A page is always readable when
 * mapped. An address space is named by the physical address of its top-level
 * table, its root.
 */

#ifndef ARCH_PAGING_H
#define ARCH_PAGING_H

#include <stdint.h>

#if defined(__i386__) || defined(CXK_HOST_TEST)
#include "arch/x86_32/paging_defs.h"   /* paddr_t, PAGE_SIZE, KERNEL_VBASE */
#else
#error "kernel/arch/paging.h: no paging for this architecture"
#endif

/* ---- what a mapping allows -------------------------------------------------- */
#define VM_WRITE     0x1u
#define VM_EXEC      0x2u   /* honoured where the MMU can refuse execution: not
                               32-bit x86 without PAE, where every page runs */
#define VM_UNCACHED  0x4u   /* device registers only - see below */
#define VM_ALL       (VM_WRITE | VM_EXEC | VM_UNCACHED)

/* ---- when VM_UNCACHED is and is not wanted ----------------------------------
 * It belongs on DEVICE REGISTER windows and essentially nowhere else.
 *
 *   YES - MMIO register BARs (an e1000's BAR0, an AHCI HBA's ABAR). These are
 *         not memory: reads have side effects, writes must reach the device in
 *         program order, and a status bit the device changes underneath us must
 *         not be served from a cache line. Emulators keep no stale copy so this
 *         costs nothing there, which is exactly why leaving it out survives
 *         testing and then misbehaves on real silicon - as a timeout or a hang,
 *         never as anything that points at caching.
 *
 *   NO  - DMA buffers. x86 keeps DMA coherent with the caches by snooping, so
 *         write-back is correct and far faster. Marking the NIC's ring buffers
 *         uncached would be a large, pointless slowdown.
 *
 *   NO  - the linear framebuffer. Uncached writes to a framebuffer are brutal;
 *         every pixel becomes a bus transaction. Write-back is wrong in theory
 *         and much better in practice. What it actually wants is
 *         write-combining, which needs PAT or an MTRR - see the note in fb.c.
 *
 *   NO  - ACPI tables and other firmware structures. They are ordinary RAM that
 *         happens to be reserved.
 */

/* Adopt the boot page tables. Call after pmm_init. */
void paging_init(void);

/* Map one page in the ACTIVE space. Both addresses page-aligned; `vm` is VM_*
   flags. Two entry points, not one, so neither can express the dangerous
   combination (security review §8):
     - kernel: any virtual address (drivers map MMIO where the BAR put it), and
       never reachable from user mode - there is no flag that would make it so;
     - user: reachable from user mode, but an address at or above KERNEL_VBASE
       is refused, so a user mapping can never name kernel memory.
   paging_map_user returns 0, or -1 if the address is not in the user half or
   `vm` holds a flag that is not VM_*. */
void paging_map_kernel(uintptr_t virt, paddr_t phys, uint32_t vm);
int  paging_map_user  (uintptr_t virt, paddr_t phys, uint32_t vm);

/* Remove the mapping of one page in the active space. */
void paging_unmap(uintptr_t virt);

/* The physical address `virt` maps to in the active space, or 0 if unmapped. */
paddr_t paging_get_phys(uintptr_t virt);
int     paging_is_user(uintptr_t virt);           /* mapped and reachable from user mode */
int     paging_is_user_writable(uintptr_t virt);  /* the above, and writable from user mode */

/* Map an arbitrary physical frame into a reserved scratch page so the kernel
   can read and write it; unmap when done. One frame at a time. */
void *paging_temp_map(paddr_t phys);
void  paging_temp_unmap(void);

/* ---- address spaces ----------------------------------------------------------
 * Each space shares the kernel half and has its own user half. */

/* Register the kernel's own space. Once, after paging_init, before any other
   space is created. */
void addr_space_init(void);

/* A new, empty space: 0 and its root in *root, or -1 when out of memory. */
int  addr_space_create(paddr_t *root);

/* Free everything a space holds, its user half and its tables. It must not be
   the active space (that panics): the last thread of its process is gone
   (cpu/proc.c), or it was never entered. */
void addr_space_release(paddr_t root);

/* Make `root` the active space. */
void addr_space_switch(paddr_t root);

paddr_t addr_space_current(void);   /* the active space */
paddr_t addr_space_kernel(void);    /* the kernel's own, where kernel threads run */

#endif
