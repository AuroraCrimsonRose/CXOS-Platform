/* /CXK/kernel/kmain.c - v5 kernel C entry
 * Aurora Tejeda / CATX SYSTEMS LLC
 *
 * Runs in the higher half. kernel.asm has set up paging, mapped the kernel high,
 * switched to a higher-half stack, and zeroed BSS. kmain brings up the kernel
 * subsystems in dependency order, then hands off to the self-tests (ktest.c).
 */

#include <stdint.h>
#include "config.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "paging.h"
#include "addr_space.h"
#include "heap.h"
#include "console.h"
#include "color.h"
#include "fb.h"
#include "mouse.h"
#include "resolution.h"
#include "format.h"
#include "sched.h"
#include "timer.h"
#include "keyboard.h"
#include "power.h"
#include "netif.h"
#include "usb.h"
#include "apic.h"
#include "acpi.h"
#include "usermode.h"
#include "ata.h"
#include "ahci.h"
#include "install.h"
#include "launch.h"
#include "disk.h"
#include "cxfs.h"
#include "pci.h"
#include "ktest.h"
#include "klogo.h"
#include "speaker.h"
#include "logging.h"

/* Mount CXFS on the data disk (first non-boot disk). Read-only by default:
   mounts an existing filesystem if present and NEVER formats, so this can't
   wipe a non-CXFS disk on real hardware. A dev build (CXK_ALLOW_DISK_WRITE)
   formats a scratch disk if it has no filesystem. The boot disk (index 0) is
   never touched. */
static void mount_cxfs(void) {
    const struct disk *fsdisk = 0;
    if (disk_count() >= 2) fsdisk = disk_get(1);   /* first non-boot disk */
    if (!fsdisk) { klog_u32("FILESYS", SEV_WARN, "DATA DISKS PRESENT: ", (uint32_t)disk_count(), LOG_COLOR_VALUE, ""); return; }

    cxfs_set_id(fsdisk->id);
    if (cxfs_mount() == 0) {
        klog("FILESYS", SEV_OK, "MOUNTED - EXISTING FILESYSTEM");
    } else {
#if CXK_ALLOW_DISK_WRITE
        klog("FILESYS", SEV_WARN, "NO FILESYSTEM - FORMATING TO CXFS");
        if (cxfs_format() == 0 && cxfs_mount() == 0)
            klog("FILESYS", SEV_OK, "CXFS FILESYSTEM MOUNTED");
        else
            klog("FILESYS", SEV_FAIL, "CXFS FILESYSTEM FORMAT AND MOUNT FAILED");
#else
        klog("FILESYS", SEV_WARN, "NO FILESYSTEM - FILESYS OPS DISABLED");
#endif
    }
    if (cxfs_is_mounted())
        klog_u32("FILESYS", SEV_OK, "CXFS FREE BLOCKS: ", (uint32_t)cxfs_free_blocks(), LOG_COLOR_VALUE, "");
}


/* VGA text-mode boot logo: the CX wordmark in ASCII, shown briefly when no
   framebuffer is available (graphics mode uses the vector logo instead). */
static const char *cx_logo_art[] = {
    "________/\\\\\\\\\\\\\\\\\\___/\\\\\\_______/\\\\\\_",
    " _____/\\\\\\////////___\\///\\\\\\___/\\\\\\/__",
    "  ___/\\\\\\/______________\\///\\\\\\\\\\\\/____",
    "   __/\\\\\\__________________\\//\\\\\\\\______",
    "    _\\/\\\\\\___________________\\/\\\\\\\\______",
    "     _\\//\\\\\\__________________/\\\\\\\\\\\\_____",
    "      __\\///\\\\\\______________/\\\\\\////\\\\\\___",
    "       ____\\////\\\\\\\\\\\\\\\\\\___/\\\\\\/___\\///\\\\\\_",
    "        _______\\/////////___\\///_______\\///__",
};
#define CX_LOGO_ROWS 9

static void cx_text_logo(void) {
    /* per-character coloring: stroke glyphs '/' and '\\' are yellow, the
       fill/underscore glyphs are white. */
    const uint8_t stroke = VGA_ATTR(VGA_YELLOW, VGA_BLACK);
    const uint8_t fill   = VGA_ATTR(VGA_WHITE,  VGA_BLACK);

    console_clear();
    console_putc('\n'); console_putc('\n');
    for (int i = 0; i < CX_LOGO_ROWS; i++) {
        console_puts("   ");          /* small left pad */
        for (const char *c = cx_logo_art[i]; *c; c++) {
            console_set_color((*c == '/' || *c == '\\') ? stroke : fill);
            console_putc(*c);
        }
        console_putc('\n');
    }
    console_set_color(VGA_ATTR(VGA_DARK_GREY, VGA_BLACK));
    console_puts("\n          CATX SYSTEMS  -  CXK v5\n");
    console_set_color(VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK));
    timer_sleep(1000);   /* ~1s pause (busy-delay: timer not running yet) */
    console_clear();
}

void kmain(void) {
    /* console first, so everything after it logs cleanly. */
    console_init();

    /* CPU core: segments + interrupts. */
    gdt_init();
    klog("GDT", SEV_OK, "GDT + TSS installed");
    idt_init();
    klog("IDT", SEV_OK, "installed");

    /* memory: physical frames -> virtual mappings -> heap. */
    pmm_init();
    klog("PMM", SEV_OK, "online");
    klog_child_u32("total pages: ", pmm_total_pages(), LOG_COLOR_VALUE, "");
    klog_child_u32("free pages: ", pmm_free_count(), VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK), "");

    paging_init();
    klog("PAGING", SEV_OK, "online (recursive page directory)");

    addr_space_init();
    klog("VMSPACE", SEV_OK, "address-space layer ready");

    heap_init();
    klog("HEAP", SEV_OK, "online (kmalloc/kfree)");

    /* initalize speakers */
    speaker_init();

    int fb_up = 0;
#if CXK_ENABLE_FB
    /* Split boot layout: the CX logo sits in the right third while the boot log
       renders in the left region. The console is rebound onto that left viewport
       for the rest of boot; a panic repaints the whole screen red (idt.c). The
       few early lines above (quiet by default) predate fb and stay on VGA. */
    if (fb_init() == 0) {
        fb_up = 1;
        uint32_t W = fb_width(), H = fb_height();
        fb_clear(fb_rgb(0, 0, 0));

        uint32_t logo_area = W / 3;                       /* right third */
        uint32_t text_w    = W - logo_area;
        uint32_t lsz       = ((logo_area < H ? logo_area : H) * 4) / 5;
        klogo_draw(text_w + (logo_area - lsz) / 2, (H - lsz) / 2, lsz);

        console_use_fb(8, 8, text_w - 16, H - 16);        /* boot log -> left */
    }
#endif

    /* no framebuffer (disabled or unavailable): show the ASCII logo briefly. */
    if (!fb_up) cx_text_logo();

    /* first VISIBLE log line - framebuffer left region if one came up, else VGA */
    klog("KERNEL", SEV_OK, "CXK v5 - higher-half kernel online");

    /* scheduler + timer (preemption available, enabled on demand). */
    sched_init();
    klog("SCHED", SEV_OK, "online");

    timer_init();
    __asm__ __volatile__("sti");          /* enable interrupts: timer can fire */
    klog("TIMER", SEV_OK, "online (PIT @ 1000 Hz), interrupts enabled");

    /* calibrate the cycle counter against the PIT so RTTs can be reported in
       microseconds rather than whole 1 ms ticks */
    timer_calibrate_tsc();
    klog_u32("TIMER", SEV_OK, "TSC calibrated, MHz: ", timer_tsc_mhz(), LOG_COLOR_VALUE, "");

    keyboard_init();
    klog("INPUT", SEV_OK, "PS/2 keyboard online (IRQ1)");

    /* PS/2 mouse. Clamped to the display so userspace gets an absolute position
       it can use directly; fb_width/fb_height return 0 in text mode, and
       mouse_init keeps its 640x480 default for a zero, so this is safe whether
       or not the framebuffer came up. */
    if (mouse_init((int)fb_width(), (int)fb_height()))
        klog("INPUT", SEV_OK, "PS/2 mouse online (IRQ12)");
    else
        klog("INPUT", SEV_WARN, "no PS/2 mouse detected");


    /* user mode: syscall gate + ring-3 fault handler. */
    usermode_init();
    usermode_register_fault_handler();
    klog("USERMODE", SEV_OK, "online (syscall gate int 0x80)");

    /* PCI bus: enumerate devices so storage/USB/NIC drivers can find their
       controllers. Must come before AHCI (which is discovered via PCI). */
    pci_init();
    klog_u32("PCI", SEV_OK, "enumerated, devices: ", (uint32_t)pci_device_count(), LOG_COLOR_VALUE, "");
    /* list what was found - class 0x02 subclass 0x00 with vendor 0x8086 is the
       e1000 the network stack looks for. Prints vendor:device and class:subclass
       so a missing or differently-classed NIC is obvious from the boot log. */
    for (unsigned i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_get(i);
        if (!d) continue;
        klog_child_u32("vendor ", (uint32_t)d->vendor_id, LOG_COLOR_VALUE, "");
        klog_child_u32("  device ", (uint32_t)d->device_id, LOG_COLOR_VALUE, "");
        klog_child_u32("  class ", (uint32_t)d->class_code, LOG_COLOR_VALUE, "");
        klog_child_u32("  sub ", (uint32_t)d->subclass, LOG_COLOR_VALUE, "");
    }

    /* network: whichever NIC is on the bus (polled). netif_init() returns 0 if
       none is. It logs which driver bound, so this line no longer names one -
       it used to say "e1000 online" unconditionally, which became a lie the
       moment a second NIC driver existed. */
    if (netif_init()) klog("NET", SEV_OK, "online");
    else              klog("NET", SEV_WARN, "no NIC found - networking offline");

    /* ACPI: enables the real shutdown (S5) and sleep paths */
    acpi_init();

    /* Interrupt controllers. Must follow acpi_init(): the MADT is the only
       description of where the APICs are and how the legacy IRQs reach them.
       Declines to a working 8259 if anything is missing, so this cannot stop a
       machine booting. */
    apic_init();

    /* storage: probe ATA + AHCI drives into the disk registry. AHCI is found via
       PCI; on a machine without one, ahci_init returns 0 harmlessly. */
    ata_init();
    ahci_init();

    /* USB AFTER the internal drives, and the order is load-bearing rather than
       stylistic. mount_cxfs() treats disk index 0 as the boot disk and never
       touches it, formatting index 1 as the data disk. Bringing USB up first
       gave a plugged-in flash drive index 0 and pushed the real boot disk to
       index 1 - so booting with a USB stick attached reformatted the disk the
       machine had just booted from. Internal storage must claim the low indices
       before anything removable can. */
    usb_init();
    klog_u32("STORAGE", SEV_OK, "online, disks: ", (uint32_t)disk_count(), LOG_COLOR_VALUE, "");

    /* filesystem: mount CXFS on the data disk (read-only unless a dev build). */
    mount_cxfs();

    /* partitioned (XBPT) disk: ensure the SYSTEM partition is a mounted CXFS
       volume - formatting it and populating /System from the STAGE payload on
       first boot. No-ops if no XBPT disk is present. */
    int launch_exec = 0;
    {
        int ir = cxk_install_boot_disk();
        if      (ir == CXK_INSTALL_DONE)      klog("INSTALL", SEV_OK,   "first boot: /System created from staged payload");
        else if (ir == CXK_INSTALL_MOUNTED)   klog("INSTALL", SEV_OK,   "SYSTEM partition mounted");
        else if (ir == CXK_INSTALL_NO_SYSTEM) klog("INSTALL", SEV_INFO, "no XBPT system disk (skipped)");
        else klog_u32("INSTALL", SEV_WARN, "first-boot install issue: ", (uint32_t)ir, LOG_COLOR_VALUE, "");

        launch_exec = (ir == CXK_INSTALL_DONE || ir == CXK_INSTALL_MOUNTED);
    }

    /* run the kernel self-tests (ktest.c) first - they create + reap their own
       ring-3 threads, so let them finish before starting the executive. */
    ktest_run();

    /* capstone: start the signed executive as the root ring-3 process (its own
       address space + scheduler thread). It runs once we idle + yield below. */
    if (launch_exec) {
        klog("EXEC", SEV_INFO, "launching /System/Boot.xoex");
        int xc = cxk_launch_executive("/System/Boot.xoex");
        if (xc >= 0) klog_u32("EXEC", SEV_OK,  "executive started, pid ", (uint32_t)xc, LOG_COLOR_VALUE, "");
        else {
            /* klog_u32 prints unsigned, so a bare -1 came out as 4294967295 -
               which reads like corruption rather than a negative error code.
               Negate for display and name the cause. */
            klog_u32("EXEC", SEV_ERR, "executive launch failed, code -",
                     (uint32_t)(-xc), LOG_COLOR_VALUE, "");
            klog_child(cxk_launch_strerror(xc));
        }
    }

    klog("KERNEL", SEV_OK, "boot complete - idle");
    for (;;) {
        yield();                       /* let the executive + its apps run */
        __asm__ __volatile__("hlt");
    }
}