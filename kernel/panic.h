// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/panic.h */
/* Aurora Tejeda / CATX Systems */

#ifndef PANIC_H
#define PANIC_H

/* Kernel-fatal: an internal invariant is broken. Renders the panic screen with
   `why`, the caller and a stack trace, saves the crash log (diag/crashlog.h),
   halts. Never for a bad request, a missing device or a faulting process -
   those have lesser tiers
   (docs/x86_32/kernel/KERNEL_CONTRACTS.md section 1). The architecture
   supplies it (arch/x86_64/idt.c), because the screen walks its stack. */
__attribute__((noreturn)) void kpanic(const char *why);

#endif
