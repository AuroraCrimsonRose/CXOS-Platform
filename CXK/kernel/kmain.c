/* /CXK/kernel/kmain.c - v5 kernel C entry
 * Aurora Tejeda / CATX SYSTEMS LLC
 *
 * Runs in the higher half. kernel.asm has set up paging, mapped the kernel high,
 * switched to a higher-half stack, and zeroed BSS. kmain brings up the kernel
 * subsystems in dependency order, then hands off to the self-tests (ktest.c).
 */

#include <stdint.h>
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "console.h"
#include "color.h"
#include "sched.h"
#include "timer.h"
#include "usermode.h"
#include "ata.h"
#include "ahci.h"
#include "disk.h"
#include "cxfs.h"
#include "pci.h"
#include "ktest.h"

void kmain(void) {
    /* console first, so everything after it logs cleanly. */
    console_init();
    console_kernel("CXK v5 - higher-half kernel online");

    /* CPU core: segments + interrupts. */
    gdt_init();
    console_boot("GDT + TSS installed");
    idt_init();
    console_boot("IDT installed");

    /* memory: physical frames -> virtual mappings -> heap. */
    pmm_init();
    console_boot("PMM online");
    console_field_u32("       total pages: ", pmm_total_pages(), VGA_ATTR(VGA_WHITE, VGA_BLACK));
    console_field_u32("   free: ",            pmm_free_count(),  VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK));
    console_newline();

    paging_init();
    console_boot("Paging online (recursive page directory)");

    heap_init();
    console_boot("Heap online (kmalloc/kfree)");

    /* scheduler + timer (preemption available, enabled on demand). */
    sched_init();
    console_boot("Scheduler online");

    timer_init();
    __asm__ __volatile__("sti");          /* enable interrupts: timer can fire */
    console_boot("Timer online (PIT @ 1000 Hz), interrupts enabled");

    /* user mode: syscall gate + ring-3 fault handler. */
    usermode_init();
    usermode_register_fault_handler();
    console_boot("Usermode online (syscall gate int 0x80)");

    /* PCI bus: enumerate devices so storage/USB/NIC drivers can find their
       controllers. Must come before AHCI (which is discovered via PCI). */
    pci_init();
    console_field_u32("PCI enumerated: devices found: ",
                      (uint32_t)pci_device_count(), VGA_ATTR(VGA_WHITE, VGA_BLACK));
    console_newline();

    /* storage: probe ATA drives, register them in the disk registry. */
    ata_init();

    /* AHCI: discovered via PCI. On machines without an AHCI controller (e.g.
       QEMU i440FX + IDE), ahci_init finds nothing and returns 0 - harmless. */
    ahci_init();

    console_field_u32("Storage online: total disks: ",
                      (uint32_t)disk_count(), VGA_ATTR(VGA_WHITE, VGA_BLACK));
    console_newline();

    /* filesystem: mount CXFS on the ATA data disk (primary slave, unit 1).
       Find it in the registry by driver+unit rather than assuming an id. */
    {
        /* Pick the data disk for CXFS. The boot disk is always registered
           first (disk index 0) - whatever driver it's on (ATA on IDE machines,
           AHCI on q35). CXFS lives on the NEXT disk (index 1+), so it works
           regardless of the underlying driver - the whole point of the disk
           abstraction. We never use index 0 (the boot disk), so cxfs_format()
           can't destroy the kernel image. */
        const struct disk *fsdisk = 0;
        if (disk_count() >= 2) fsdisk = disk_get(1);   /* first non-boot disk */

        if (fsdisk) {
            cxfs_set_id(fsdisk->id);
            if (cxfs_mount() == 0) {
                console_boot("CXFS mounted (existing filesystem)");
            } else {
                console_warn("CXFS: no filesystem found - formatting");
                if (cxfs_format() == 0 && cxfs_mount() == 0)
                    console_boot("CXFS formatted + mounted");
                else
                    console_err("CXFS: format/mount FAILED");
            }
            if (cxfs_is_mounted()) {
                console_field_u32("       free blocks: ", cxfs_free_blocks(),
                                  VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK));
                console_newline();
            }
        } else {
            console_warn("CXFS: no disk available to mount");
        }
    }

    /* run the kernel self-tests (ktest.c). */
    ktest_run();

    console_kernel("boot complete - idle");
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}