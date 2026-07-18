ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}
SECTIONS {
    . = 0x00400000;
    .text   : { *(.text*) }         :text
    .rodata : { *(.rodata*) }       :text
    . = ALIGN(0x1000);
    .data   : { *(.data*) }         :data
    .bss    : { *(.bss*) *(COMMON) } :data
}
