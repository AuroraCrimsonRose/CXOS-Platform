/* /os/apps/hello/hello.ld - link a .xuex in the user half (its own space)
 *
 * Page-aligned per section for the same reason as executive.ld: the CXEX
 * loader maps one section at a time with that section's protection, so two
 * sections sharing a page lose the earlier one's mapping. ld.lld packs them
 * tightly unless the script says otherwise.
 */
ENTRY(_start)
SECTIONS {
    . = 0x00400000;
    .text   : { *(.text*) }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) }
    . = ALIGN(0x1000);
    .data   : { *(.data*) }
    . = ALIGN(0x1000);
    .bss    : { *(.bss*) *(COMMON) }
}
