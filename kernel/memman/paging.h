// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/memman/paging.h */
/* Aurora Tejeda */
/*
 * Virtual memory / paging - x86 32-bit, 2-level (Page Directory -> Page Table),
 * 4 KB pages.
 *
 * v1: identity-maps the kernel and low memory, then enables the MMU, so
 * virtual == physical for existing memory and the transition is seamless.
 * Higher-half kernel mapping comes as the next step.
 */

#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE        4096u
#define PAGE_ENTRIES     1024          /* entries per directory / table */

/* page flags (low bits of a PDE/PTE) */
#define PAGE_PRESENT     0x001
#define PAGE_WRITE       0x002
#define PAGE_USER        0x004         /* 1 = accessible from ring 3 */
#define PAGE_WRITE_THRU  0x008         /* PWT: write-through instead of write-back */
#define PAGE_NO_CACHE    0x010         /* PCD: do not cache this page at all */

/* ---- when PAGE_NO_CACHE is and is not wanted ------------------------------
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
 *
 * Note this bit only has to be set in the PTE: for a 4 KB page the PDE's PCD
 * governs caching of the page TABLE, not of the page it maps.
 */

/* set up identity mapping and enable paging. Call after pmm_init. */
void paging_init(void);

/* map one virtual page to one physical frame with the given flags.
   Both addresses must be page-aligned. */
/* Two entry points, not one, so neither can express the dangerous combination
   (security review §8):
     - kernel: any virtual address (drivers map MMIO where the BAR put it), but
       PAGE_USER is refused, so a kernel mapping is never reachable from ring 3;
     - user: PAGE_USER is added for you, but an address at or above KERNEL_VBASE
       is refused, so a ring-3 mapping can never name kernel memory.
   paging_map_user returns 0, or -1 if the address is not in the user half. */
void paging_map_kernel(uint32_t virt, uint32_t phys, uint32_t flags);
int  paging_map_user  (uint32_t virt, uint32_t phys, uint32_t flags);

/* remove a mapping for a virtual page. */
void paging_unmap(uint32_t virt);

/* return the physical address a virtual address maps to, or 0 if unmapped. */
uint32_t paging_get_phys(uint32_t virt);
int      paging_is_user(uint32_t virt);           /* present + ring-3 accessible in active space */
int      paging_is_user_writable(uint32_t virt);  /* the above, and writable by ring 3 */

/* temporarily map an arbitrary physical frame into a reserved scratch page so
   the kernel can read/write it; unmap when done. One frame at a time. */
void *paging_temp_map(uint32_t phys);
void  paging_temp_unmap(void);

/* register a hook called when paging_map creates a new shared-kernel-half PDE,
   so the address-space layer can propagate it into live spaces. */
void  paging_set_pde_hook(void (*fn)(uint32_t index, uint32_t value));

#endif