/* /os/executive/executive.ld - link the .xoex in the user half */
ENTRY(_start)
SECTIONS {
    . = 0x00400000;                 /* user-half load base (below kernel 0xC0000000) */
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}
