/* /CXLite/kernel/cpu/fpu.c */
/* Aurora Tejeda */
/*
 * FPU + SSE bring-up.
 *
 * On x86 the FPU and SSE are disabled in a way that traps by default. Before
 * any floating-point instruction can run we must:
 *   CR0.EM (bit 2) = 0   - "emulation" off; let FPU/SSE execute natively
 *                          (if 1, FP instructions trap as #7 Device Not Avail)
 *   CR0.MP (bit 1) = 1   - "monitor coprocessor"
 *   CR0.TS (bit 3) = 0   - "task switched" off (no lazy-FP trap for now)
 *   CR4.OSFXSR    (bit 9)  = 1 - OS supports fxsave/fxrstor; enables SSE
 *   CR4.OSXMMEXCPT(bit 10) = 1 - unmasked SSE FP exceptions reported as #19
 * then `fninit` to set a known x87 state. After this, scalar float/double and
 * SSE math work. (Single-threaded for now, so no per-task FPU save/restore;
 * that comes with the future process model via the #7 handler.)
 */

#include "fpu.h"
#include <stdint.h>

void fpu_init(void) {
    uint32_t cr0, cr4;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2);     /* clear EM */
    cr0 |=  (1u << 1);     /* set   MP */
    cr0 &= ~(1u << 3);     /* clear TS */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 9);      /* OSFXSR    */
    cr4 |= (1u << 10);     /* OSXMMEXCPT */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile ("fninit");
}