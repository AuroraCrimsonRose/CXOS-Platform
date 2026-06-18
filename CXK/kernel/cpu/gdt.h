/* /CXK/kernel/cpu/gdt.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * Kernel-owned GDT + TSS.
 *
 * The bootloader sets up a minimal 3-entry GDT (null, ring-0 code, ring-0 data)
 * to get into protected mode. For ring 3 we need more: ring-3 code/data
 * segments and a TSS (so the CPU knows which kernel stack to switch to when an
 * interrupt occurs while running in user mode). gdt_init() builds and loads a
 * fresh 6-entry GDT, keeping the ring-0 selectors at the SAME values (0x08 /
 * 0x10) the bootloader used, so all existing kernel code keeps working.
 */

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* segment selectors (index << 3 | RPL) */
#define KERNEL_CODE_SEL  0x08      /* ring 0 */
#define KERNEL_DATA_SEL  0x10      /* ring 0 */
#define USER_CODE_SEL    0x1B      /* ring 3: index 3 (0x18) | RPL 3 */
#define USER_DATA_SEL    0x23      /* ring 3: index 4 (0x20) | RPL 3 */
#define TSS_SEL          0x28      /* index 5 */

/* build + load the kernel GDT and TSS, reload segment registers, ltr the TSS. */
void gdt_init(void);

/* set the kernel stack the CPU switches to on a ring3->ring0 transition.
   (updates tss.esp0; call before entering user mode or per context switch.) */
void tss_set_kernel_stack(uint32_t esp0);

#endif