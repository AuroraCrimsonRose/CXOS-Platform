/* /kernel/kernel/linker.ld - v5 higher-half kernel
 * Aurora Tejeda / CATX Systems
 *
 * Higher-half layout: the kernel is LINKED at a high virtual address
 * (0xC0000000 + 1MB) but LOADED physically at 1MB (0x100000). The AT()
 * directive expresses this split:
 *   VMA (where symbols resolve / code "thinks" it runs): 0xC0100000+
 *   LMA (where the bytes physically sit on disk / in RAM): 0x00100000+
 *
 * Stage 2 loads the kernel image to physical 0x100000 and jumps to the
 * PHYSICAL entry. The entry stub (kernel.asm) sets up paging to map
 * physical 1MB -> virtual 0xC0100000 (plus a temporary identity map so the
 * instruction right after enabling paging doesn't fault), then jumps high.
 */

ENTRY(kernel_entry)

KERNEL_VBASE = 0xC0000000;      /* higher-half virtual base (3 GB) */
KERNEL_PHYS  = 0x00100000;      /* physical load address (1 MB) */

SECTIONS
{
    /* Code runs at VMA 0xC0100000 but is placed at LMA 0x00100000. */
    . = KERNEL_VBASE + KERNEL_PHYS;

    /* expose the virtual base to asm so it can compute phys = virt - VBASE */
    __kernel_vbase = KERNEL_VBASE;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VBASE)
    {
        __text_start = .;
        *(.multiboot)          /* if a header is ever added, keep it first */
        *(.text*)
        __text_end = .;
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VBASE)
    {
        __rodata_start = .;
        *(.rodata*)
        __rodata_end = .;
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VBASE)
    {
        __data_start = .;
        *(.data*)
        __data_end = .;
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VBASE)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    }

    /* Page tables: NOBITS like .bss, but OUTSIDE __bss_start..__bss_end so the
       kernel's BSS-zeroing loop never touches the live page directory/table
       (CR3 points at them; zeroing them would triple-fault). */
    .pagetables ALIGN(4K) : AT(ADDR(.pagetables) - KERNEL_VBASE)
    {
        *(.pagetables)
    }

    /* highest virtual address used - the entry stub maps up to here */
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
