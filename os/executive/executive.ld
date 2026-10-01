/* /os/executive/executive.ld - link the .xoex in the user half
 *
 * Each section is page-aligned, and that is load-bearing, not tidiness: the
 * CXEX loader maps section by section, one page-table entry per page, applying
 * that section's own protection. Two sections sharing a page means the second
 * one's mapping replaces the first's PTE - frame and all - so the bytes of the
 * earlier section simply vanish from that page.
 *
 * GNU ld used to hide this by aligning LOAD segments to the page size of its
 * own accord. ld.lld honours the script exactly, so without these ALIGNs it
 * packs .rodata directly behind .text: .text landed at 0x00400000 with
 * .rodata at 0x004002BC, both in page 0x00400000, and mapping .rodata took the
 * entry point's page away with it. The executive then faulted on its first
 * instruction. Keep every section on its own page.
 */
ENTRY(_start)
SECTIONS {
    . = 0x00400000;                 /* user-half load base (below kernel 0xC0000000) */
    .text   : { *(.text*) }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) }
    . = ALIGN(0x1000);
    .data   : { *(.data*) }
    . = ALIGN(0x1000);
    .bss    : { *(.bss*) *(COMMON) }
}
