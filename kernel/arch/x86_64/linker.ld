/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems) */
/* /kernel/arch/x86_64/linker.ld */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 kernel: linked in the top 2 GiB (clang's kernel code model),
   placed at physical 1 MiB, with every address at KERNEL_VBASE + physical
   (arch/x86_64/paging_defs.h). Stage 2 (boot/long.asm) refuses a PT_LOAD
   segment that breaks that rule, so it is the one rule this script keeps. */

ENTRY(_start)

KERNEL_VBASE = 0xFFFFFFFF80000000;
KERNEL_PHYS  = 0x00100000;

SECTIONS
{
    . = KERNEL_VBASE + KERNEL_PHYS;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VBASE)
    {
        __text_start = .;
        *(.text.entry)
        *(.text .text.*)
        __text_end = .;
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VBASE)
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VBASE)
    {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VBASE)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
