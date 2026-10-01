/* /os/apps/hello/hello.ld - link a .xuex in the user half (its own space) */
ENTRY(_start)
SECTIONS {
    . = 0x00400000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}
