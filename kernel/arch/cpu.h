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

/* The CPU's own tables: segments, the interrupt table, the task state; then
   the feature table (what this CPU has). Called once, early, before
   interrupts are enabled. */
void arch_cpu_init(void);

/* The stack the CPU switches to when an interrupt arrives from user mode.
   The scheduler sets it on every switch to the incoming thread's own. */
void arch_kernel_stack_set(uintptr_t top);

/* The stack used when the running thread has none of its own (a kernel-only
   thread). */
uintptr_t arch_kernel_stack_default(void);

/* The stack an interrupt from user mode would switch to right now. */
uintptr_t arch_kernel_stack_current(void);

/* Move onto the stack whose top is `top` and call fn there, with no frame
   above it (a stack trace ends at fn). Never returns. */
__attribute__((noreturn)) void arch_run_on_stack(uintptr_t top, void (*fn)(void));

/* Reset the processor by the means of last resort, when the platform's own
   reset did nothing. Never returns. */
__attribute__((noreturn)) void arch_cpu_reset(void);

#endif
