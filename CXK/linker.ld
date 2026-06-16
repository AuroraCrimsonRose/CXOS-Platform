ENTRY(kernel_entry)
SECTIONS
{
    . = 0x10000;
    .text : {
        *(.text.kernel_entry)
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }
}