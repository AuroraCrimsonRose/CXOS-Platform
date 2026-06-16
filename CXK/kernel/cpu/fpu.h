/* /CXLite/kernel/cpu/fpu.h */
/* Aurora Tejeda */
/* Enable the x87 FPU and SSE so the kernel can use floating-point math.
   Must be called early in kmain, before ANY float/SSE code runs - otherwise
   the first such instruction faults (the FPU/SSE start disabled on x86). */

#ifndef FPU_H
#define FPU_H

void fpu_init(void);

#endif