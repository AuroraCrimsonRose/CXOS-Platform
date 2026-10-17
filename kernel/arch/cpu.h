// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/cpu.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The processor, as generic code sees it (docs/kernel/CX_KERNEL_PLAN.md
 * section 2.2). Each architecture implements this in kernel/arch/<arch>/.
 */

#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <stdint.h>

/* The CPU's own tables: segments, the interrupt table, the task state.
   Called once, early, before interrupts are enabled. */
void arch_cpu_init(void);

/* The stack the CPU switches to when an interrupt arrives from user mode.
   The scheduler sets it on every switch to the incoming thread's own. */
void arch_kernel_stack_set(uintptr_t top);

/* The stack used when the running thread has none of its own (a kernel-only
   thread). */
uintptr_t arch_kernel_stack_default(void);

#endif
