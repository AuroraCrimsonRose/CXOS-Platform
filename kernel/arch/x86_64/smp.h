// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/smp.h */
/* Aurora Tejeda / CATX Systems */
/* Inside arch/x86_64 only: what smp.c (arch/smp.h's implementation) needs of
   the rest of the arch, and gives it. */
#ifndef ARCH_X86_64_SMP_H
#define ARCH_X86_64_SMP_H

#include <stdint.h>
#include "gdt.h"
#include "pagetable.h"
#include "cpu/percpu.h"

/* The page a STARTUP IPI names (smp_tramp.asm's SMP_TRAMP_PHYS). */
#define SMP_TRAMP_PHYS 0x8000u

/* cpu.c: an AP's tables, GS and MSRs, on the AP. -1 past CPU_MAX. */
int arch_cpu_init_ap(struct cpu *c, const uintptr_t ist_lo[IST_STACKS], uintptr_t rsp0_top);

/* paging.c: the root an AP turns paging on with (below 4 GiB; the kernel
   half and `page` at its own address); addr_space_release frees it. */
int paging_boot_root(paddr_t page, paddr_t *root);

/* paging.c: load `root` on CPU `c`, recording it first in c->space for the
   shootdown's targeting (arch/smp.h). addr_space_switch is this for the
   calling CPU; an AP calls it before its GS names its struct cpu. */
void addr_space_switch_on(struct cpu *c, paddr_t root);

/* idt.c calls it for any interrupt an AP takes: counted (the IPI's serves
   the shootdowns waiting), then the EOI. */
void smp_ap_interrupt(uint32_t vector);

/* The self-tests' view (ktest_arch.c): each AP's stacks and how many of its
   timer's interrupts it took. */
struct smp_ap_stacks {
    uintptr_t idle_base, idle_top;
    uintptr_t rsp0_base, rsp0_top;
    uintptr_t ist_base[IST_STACKS];
};
const struct smp_ap_stacks *smp_stacks_of(unsigned index);
uint32_t smp_timer_hits(unsigned index);

#endif
