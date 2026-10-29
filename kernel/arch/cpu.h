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

/* The CXEX arch_target this kernel can run (lib/format/cxex.h). The loader
   refuses an image built for any other: an x86-32 image entered on x86-64
   would run its 32-bit code as 64-bit instructions (there is no compat mode). */
#if defined(__x86_64__) || defined(CXK_HOST_TEST)
#define ARCH_CXEX_TARGET 2u   /* CXEX_ARCH_X86_64 */
#else
#error "kernel/arch/cpu.h: no CXEX arch_target for this architecture"
#endif

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

/* Switch threads: save the running thread's callee-saved state on its own
   stack and its stack pointer in *old_sp, then resume the thread whose saved
   stack pointer is new_sp. Returns when something switches back. */
void context_switch(uintptr_t *old_sp, uintptr_t new_sp);

/* Lay out a new thread's first frame at the top of its kernel stack, as
   context_switch leaves a thread it switched away from, so the first switch
   to it calls `launch` with interrupts enabled. Returns the saved stack
   pointer to give context_switch. */
uintptr_t arch_thread_frame(uintptr_t stack_top, void (*launch)(void));

/* Move onto the stack whose top is `top` and call fn there, with no frame
   above it (a stack trace ends at fn). Never returns. */
__attribute__((noreturn)) void arch_run_on_stack(uintptr_t top, void (*fn)(void));

/* Reset the processor by the means of last resort, when the platform's own
   reset did nothing. Never returns. */
__attribute__((noreturn)) void arch_cpu_reset(void);

#endif
