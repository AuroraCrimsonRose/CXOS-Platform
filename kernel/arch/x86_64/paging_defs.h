// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/paging_defs.h */
/* Aurora Tejeda / CATX Systems */
/* The sizes arch/paging.h exports, for x86-64 with four-level paging
   (docs/kernel/CX_KERNEL_PLAN.md section 2.4). Include arch/paging.h, not this. */

#ifndef ARCH_X86_64_PAGING_DEFS_H
#define ARCH_X86_64_PAGING_DEFS_H

#include <stdint.h>

/* A physical address. 52 bits is the architectural limit. */
typedef uint64_t paddr_t;

#define PAGE_SIZE        4096u
#define PAGE_TABLE_SPAN  0x00200000u   /* what one page table maps: 2 MiB */

/* The kernel image: the top 2 GiB, so clang's kernel code model applies.
   Physical P of the image, and of the low window the loader maps with it, is
   at KERNEL_VBASE + P, as on x86-32. */
#define KERNEL_VBASE     0xFFFFFFFF80000000ull

/* The user half ends where the canonical lower half does. */
#define USER_VLIMIT      0x0000800000000000ull

/* The kernel half, by PML4 entry (CX_KERNEL_PLAN section 2.4). All 256
   entries are made at boot (paging.c), so every region below exists in every
   address space from the start.
     256-383  the direct map: physical P at DIRECT_MAP_BASE + P, RAM only,
              no-execute, 2 MiB pages where they fit (64 TiB)
     384-447  the physical window: what paging_map_phys maps, device registers
              and firmware tables, at PHYS_WINDOW_BASE + P (32 TiB)
     448-453  the fixed regions, one entry (512 GiB) each: those generic code
              names, and the arch self-tests' scratch page
     511      the kernel image, in its top 2 GiB */
#define DIRECT_MAP_BASE  0xFFFF800000000000ull
#define DIRECT_MAP_SIZE  0x0000400000000000ull
#define PHYS_WINDOW_BASE 0xFFFFC00000000000ull
#define PHYS_WINDOW_SIZE 0x0000200000000000ull

#define KVA_HEAP         0xFFFFE00000000000ull   /* growing up to KVA_OCACHE */
#define KVA_OCACHE       0xFFFFE08000000000ull
#define KVA_KSTACKS      0xFFFFE10000000000ull
#define KVA_AHCI_DMA     0xFFFFE18000000000ull   /* the drivers' DMA windows */
#define KVA_E1000_DMA    0xFFFFE18001000000ull
#define KVA_RTL_DMA      0xFFFFE18002000000ull
#define KVA_EHCI_DMA     0xFFFFE18003000000ull   /* controller i: + i * 0x10000 */
#define KVA_XHCI_DMA     0xFFFFE18004000000ull   /* controller i: + i * 0x20000 */
#define KVA_OHCI_DMA     0xFFFFE18005000000ull   /* controller i: + i * 0x10000 */
#define KVA_FRAMEBUFFER  0xFFFFE20000000000ull
#define KVA_KTEST_SCRATCH 0xFFFFE28000000000ull  /* one page the arch self-tests map */


#endif
