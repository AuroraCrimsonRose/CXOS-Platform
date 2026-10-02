// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/cpu/fpu.h */
/* Aurora Tejeda */
/* Enable the x87 FPU and SSE so the kernel can use floating-point math.
   Must be called early in kmain, before ANY float/SSE code runs - otherwise
   the first such instruction faults (the FPU/SSE start disabled on x86).

   NOT CURRENTLY BUILT OR CALLED. See the header comment in fpu.c: nothing in
   the kernel uses floating point, and context_switch does not save or restore
   FPU/SSE state, so enabling FP before that exists would let two preemptible
   ring-3 processes corrupt each other's registers. Do the save/restore work
   first. */

#ifndef FPU_H
#define FPU_H

void fpu_init(void);

/* Valid only after fpu_init(). Both report what CPUID found, not what was
   requested, so a kernel that grows FP support can degrade rather than fault. */
int  fpu_has_x87(void);
int  fpu_has_sse(void);

#endif
