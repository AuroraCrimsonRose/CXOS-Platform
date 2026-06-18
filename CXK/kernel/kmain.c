/* /CXK/kernel/kmain.c - v5 kernel C entry
 * Aurora Tejeda / CATX SYSTEMS LLC
 *
 * Runs in the higher half. kernel.asm has set up paging, mapped the kernel high,
 * switched to a higher-half stack, and zeroed BSS. This brings up the kernel
 * subsystems in dependency order, logging progress through the console.
 */

#include <stdint.h>
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "console.h"
#include "color.h"

void kmain(void) {
    /* console first, so everything after it logs cleanly. */
    console_init();
    console_kernel("CXK v5 - higher-half kernel online");

    /* CPU core */
    gdt_init();
    console_boot("GDT + TSS installed");

    idt_init();
    console_boot("IDT installed (faults produce a panic dump)");

    /* physical memory */
    pmm_init();
    console_boot("PMM online");
    console_puts("       total pages: "); console_put_u32(pmm_total_pages());
    console_puts("   free: ");            console_put_u32(pmm_free_count());
    console_newline();

    /* paging: adopt boot page dir + recursive mapping */
    paging_init();
    console_boot("Paging: recursive page directory installed");

    /* paging proof: map a scratch frame, write+read it back */
    {
        uint32_t test_virt = 0xCF000000;
        uint32_t frame = (uint32_t)pmm_alloc();
        if (frame) {
            paging_map(test_virt, frame, PAGE_WRITE);
            volatile uint32_t *p = (volatile uint32_t *)test_virt;
            *p = 0xCAFEBABE;
            if (*p == 0xCAFEBABE && paging_get_phys(test_virt) == frame)
                console_boot("Paging: map/write/read round-trip OK");
            else
                console_err("Paging: round-trip FAILED");
            paging_unmap(test_virt);
            pmm_free((void *)frame);
        } else {
            console_err("Paging: pmm_alloc failed");
        }
    }

    /* heap */
    heap_init();
    {
        char *a = (char *)kmalloc(64);
        char *b = (char *)kmalloc(128);
        char *c = (char *)kmalloc(64);
        int ok = (a && b && c);
        if (ok) {
            for (int i = 0; i < 64; i++)  a[i] = (char)i;
            for (int i = 0; i < 128; i++) b[i] = (char)(i ^ 0x5A);
            for (int i = 0; i < 64; i++)  c[i] = (char)(i + 1);
            for (int i = 0; i < 64; i++)  if (a[i] != (char)i)         ok = 0;
            for (int i = 0; i < 128; i++) if (b[i] != (char)(i ^ 0x5A)) ok = 0;
            for (int i = 0; i < 64; i++)  if (c[i] != (char)(i + 1))    ok = 0;
            kfree(b);
            char *d = (char *)kmalloc(100);
            if (!d) ok = 0;
            kfree(a); kfree(c); kfree(d);
        }
        if (ok) console_boot("Heap: kmalloc/kfree round-trip OK");
        else    console_err("Heap: kmalloc/kfree FAILED");
    }

    console_kernel("boot complete - idle");

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}