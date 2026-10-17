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

#endif
