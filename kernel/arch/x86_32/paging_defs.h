// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_32/paging_defs.h */
/* Aurora Tejeda / CATX Systems */
/* The sizes arch/paging.h exports, for 32-bit x86 without PAE. Include
   arch/paging.h, not this. */

#ifndef ARCH_X86_32_PAGING_DEFS_H
#define ARCH_X86_32_PAGING_DEFS_H

#include <stdint.h>

/* A physical address. 32 bits: without PAE the page tables reach no higher. */
typedef uint32_t paddr_t;

#define PAGE_SIZE        4096u

/* The higher-half base: the kernel lives at and above it, user space below.
   The ONE C definition. entry.asm (KERNEL_VBASE equ) and linker.ld restate it
   because they cannot include a C header (docs/x86_32/kernel/KERNEL_CONTRACTS.md
   section 2). */
#define KERNEL_VBASE     0xC0000000u

/* The first address past the user half: user space is everything below the
   kernel. */
#define USER_VLIMIT      KERNEL_VBASE

/* The kernel half's fixed regions, laid out in one place so no two overlap
   (drivers/video/fb.c records the time two did). Generic code names them;
   each architecture places them. Above them, 0xFFC00000 up is the recursive
   page-table window (paging.c). */
#define KVA_KSTACKS      0xCF000000u   /* 16 MB of kernel stacks (memman/kstack.h) */
#define KVA_HEAP         0xD0000000u   /* the heap, growing up to KVA_OCACHE */
#define KVA_OCACHE       0xD8000000u   /* 64 MB of object caches (memman/ocache.h) */
#define KVA_AHCI_DMA     0xE0000000u   /* the drivers' DMA windows */
#define KVA_E1000_DMA    0xE1000000u
#define KVA_RTL_DMA      0xE2000000u
#define KVA_FRAMEBUFFER  0xF0000000u   /* about 252 MB, to the recursive window */


#endif
