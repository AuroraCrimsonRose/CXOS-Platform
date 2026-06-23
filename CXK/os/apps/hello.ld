/* /CXK/os/apps/hello/hello.ld - link a .xcex in the user half (its own space) */
ENTRY(_start)
SECTIONS {
    . = 0x00400000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}
