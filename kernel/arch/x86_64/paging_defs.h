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

/* The kernel image: the top 2 GiB, so clang's kernel code model applies.
   Physical P of the image, and of the low window the loader maps with it, is
   at KERNEL_VBASE + P, as on x86-32. */
#define KERNEL_VBASE     0xFFFFFFFF80000000ull

/* The user half ends where the canonical lower half does. */
#define USER_VLIMIT      0x0000800000000000ull

#endif
