// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/pat.h */
/* Aurora Tejeda / CATX Systems */
/* The page attribute table: entry 4 becomes write-combining, the rest are
   set as the CPU starts them (WB, WT, UC-, UC), so PCD and PWT keep their old
   meaning and VM_WC is the PAT bit alone (pagetable.h). Entry 4 as Serenity
   chose (Kernel/Arch/x86_64/Processor.cpp) and
   Haiku (src/system/kernel/arch/x86/arch_cpu.cpp, init_pat); XNU takes entry
   6 instead (osfmk/i386/mtrr.c, pat_init). */

#ifndef ARCH_X86_64_PAT_H
#define ARCH_X86_64_PAT_H

#include <stdint.h>

#define MSR_PAT          0x277u
#define PAT_TYPE_UC      0x00u
#define PAT_TYPE_WC      0x01u
#define PAT_TYPE_WT      0x04u
#define PAT_TYPE_WB      0x06u
#define PAT_TYPE_UC_     0x07u   /* UC-: uncached, unless an MTRR says WC */
#define PAT_WC_ENTRY     4u

/* The table the CPU starts with: entries 0-7 are WB WT UC- UC, twice. */
#define PAT_POWER_ON     0x0007040600070406ull

/* CXK's: the same with entry 4 write-combining. Written whole rather than
   entry 4 alone, so what firmware left in the others is not trusted. */
#define PAT_CXK          ((PAT_POWER_ON & ~(0xFFull << (PAT_WC_ENTRY * 8))) | \
                          ((uint64_t)PAT_TYPE_WC << (PAT_WC_ENTRY * 8)))

/* Program this CPU's table, if it has one (CPUID 1 EDX bit 16). Every CPU
   must hold the same table, so an application processor calls it too, before
   it touches a VM_WC page. No translation uses entry 4 before this, so no
   cache or TLB holds a line of the old type to flush. */
void pat_init(void);

/* 1 once entry 4 is write-combining on this CPU; paging_map_kernel drops
   VM_WC until then, and on a CPU with no table. */
int  pat_wc_ready(void);

#endif
