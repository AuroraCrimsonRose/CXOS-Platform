// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/kmain.c - v5 kernel C entry
 * Aurora Tejeda / CATX Systems
 *
 * Runs in the higher half. the loader (boot/long.asm) has set up paging and mapped the kernel high,
 * zeroing BSS; arch/x86_64/entry.asm has switched to a higher-half stack. kmain brings up the kernel
 * subsystems in dependency order, then hands off to the self-tests (ktest.c).
 */

#include <stdint.h>
#include "config.h"

/* Generated into the build directory from versions.json
   (tools/cmake/CMakeLists.txt), so the banner cannot disagree with the registry.
   The fallback is for a bare host compile with no CMake, and it SAYS it is a
   fallback rather than quietly printing a number that might be wrong - the
   banner was the literal "CXK v5" before, which is exactly how it stayed at v5
   through every change the kernel ever had. */
#if defined(__has_include)
#  if __has_include("cxk_version.h")
#    include "cxk_version.h"
#  endif
#endif
#ifndef CXK_VERSION_STR
#define CXK_VERSION_STR "0.0.0-nocmake"
#endif
#include "arch/cpu.h"
#include "arch/cpu_ops.h"
#include "arch/irq.h"
#include "panic.h"
#include "pmm.h"
#include "arch/paging.h"
#include "heap.h"
#include "kstack.h"
#include "memman/ocache.h"
#include "console.h"
#include "serial.h"
#include "color.h"
#include "fb.h"
#include "mouse.h"
#include "resolution.h"
#include "format.h"
#include "sched.h"
#include "timer.h"
#include "power.h"
#include "netif.h"
#include "usb.h"
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
#include "kconfig.h"
#include "klogo.h"
#include "platform/platform.h"
#include "logging.h"
#include "boot/cxbi.h"
#include "diag/crashlog.h"

/* Which disks this boot may write (CXK_AUTO_INSTALL, config.h). Before
   anything writes: the filesystem, the install and the crash log all come
   after. Returns the CXOS disk's id, or -1. The disk is the one the loader
   read the kernel from, named by the id in its partition table. */
static int guard_disks(void) {
    int home = cxk_find_boot_disk(cxbi()->boot_disk_id);
    if (home == CXK_BOOT_DISK_UNNAMED)
        klog("STORAGE", SEV_WARN, "the loader did not say which disk it booted from");
    else if (home == CXK_BOOT_DISK_TWICE)
        klog("STORAGE", SEV_WARN, "two disks carry this system's id: neither is trusted");
    else if (home == CXK_BOOT_DISK_NONE)
        klog("STORAGE", SEV_WARN, "the disk the loader booted from is not present");
    if (home < 0) home = -1;
#if CXK_AUTO_INSTALL
    klog("STORAGE", SEV_WARN, "automatic install on: other disks may be formatted");
#else
    disk_restrict_writes(home);
    if (home >= 0)
        klog_u32("STORAGE", SEV_OK, "writes limited to the CXOS disk, id: ",
                 (uint32_t)home, LOG_COLOR_VALUE, "");
    else
        klog("STORAGE", SEV_WARN, "no CXOS disk: every disk is read-only");
    /* Each disk refused, by size: what `cxk verify-boot --foreign-disk` looks
       for to know its disk was the one seen, not merely some second disk. */
    for (unsigned i = 0; i < disk_count(); i++) {
        const struct disk *d = disk_get(i);
        if (d && d->id != home)
            klog_u32("STORAGE", SEV_INFO, "left read-only, sectors: ",
                     (uint32_t)d->sectors, LOG_COLOR_VALUE, "");
    }
#endif
    return home;
}

/* Mount CXFS on the data disk (index 1). Read-only by default: mounts an
   existing filesystem if present and NEVER formats, so this can't wipe a
   non-CXFS disk on real hardware. Only with automatic install on (and
   CXK_ALLOW_DISK_WRITE) is a disk with no filesystem formatted. The CXOS disk
   itself is never taken for one: whole-disk CXFS there would overwrite its
   partition table. Nor, when the CXOS disk is index 1, is index 0 taken in its
   place: that is the machine's own first drive. */
static void mount_cxfs(int home) {
    const struct disk *fsdisk = 0;
    if (disk_count() >= 2) fsdisk = disk_get(1);
    if (!fsdisk || fsdisk->id == home) { klog_u32("FILESYS", SEV_WARN, "DATA DISKS PRESENT: ", (uint32_t)disk_count(), LOG_COLOR_VALUE, ""); return; }

    cxfs_set_id(fsdisk->id);
    if (cxfs_mount() == 0) {
        klog("FILESYS", SEV_OK, "MOUNTED - EXISTING FILESYSTEM");
    } else {
#if CXK_ALLOW_DISK_WRITE && !CXK_AUTO_INSTALL
        klog("FILESYS", SEV_INFO, "NO FILESYSTEM - LEFT ALONE (automatic install off)");
#elif CXK_ALLOW_DISK_WRITE
        klog("FILESYS", SEV_WARN, "NO FILESYSTEM - FORMATING TO CXFS");
        /* Stamp a label while we are formatting it. Without one the volume
           mounts under a generated "VolumeN", which tells a reader nothing and
           changes if the disks are enumerated in a different order. */
        if (cxfs_format_labeled(0, (16u * 1024 * 1024) / 4096u, "Data") == 0 &&
            cxfs_mount() == 0)
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

/* How often the idle loop re-saves a grown system log to the crash area. */
#define BOOTLOG_SAVE_MS 5000u

/* How often the idle loop looks at the USB root ports for a device plugged in
   or pulled out (usb_poll). A keyboard EHCI hands to its companion connects
   there after the boot has looked, and one plugged in later never connects
   during a boot at all. */
#define USB_POLL_MS 250u

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
    console_puts("\n          CATX SYSTEMS  -  CXK v" CXK_VERSION_STR "\n");
    console_set_color(VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK));
    timer_sleep(1000);   /* ~1s pause (busy-delay: timer not running yet) */
    console_clear();
}

void kmain_late(void) __attribute__((noreturn, used));

/* `cxbi_phys` is the loader's handover (boot/cxbi.h), passed by the entry
   stub. */
void kmain(uintptr_t cxbi_phys) {
    /* Serial before the console, so the first console line is already being
       mirrored. The probe prints nothing and is a no-op without a UART. */
    serial_init();

    /* console first, so everything after it logs cleanly. */
    console_init();

    /* the handover, before anything asks about memory, the screen or ACPI */
    cxbi_init(cxbi_phys);

    /* CPU core: the processor's own tables, then interrupts, then the feature
       table, before anything that might ask what the CPU has. */
    arch_cpu_init();

    /* memory: physical frames and the kernel's page tables, in the order the
       architecture needs (arch/paging.h) -> heap. */
    arch_memory_init();
    klog("PMM", SEV_OK, "online");
    klog_child_u32("total pages: ", pmm_total_pages(), LOG_COLOR_VALUE, "");
    klog_child_u32("free pages: ", pmm_free_count(), VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK), "");
    klog("PAGING", SEV_OK, "online");

    addr_space_init();
    klog("VMSPACE", SEV_OK, "address-space layer ready");

    /* before any address space but the kernel's exists, as kstack_init below;
       and before the heap, whose small sizes come from these caches */
    ocache_sys_init();
    klog("OCACHE", SEV_OK, "object cache window reserved");

    heap_init();
    klog("HEAP", SEV_OK, "online (kmalloc/kfree)");

    /* before any address space but the kernel's exists: see kstack_init */
    kstack_init();
    klog("KSTACK", SEV_OK, "guarded kernel stacks ready");

    /* Leave the boot stack. arch/x86_64/entry.asm could only give kmain a stack in .bss,
       with nothing below it, and kmain goes on to become thread 0 - the thread
       the self-tests and every file path run on. From here it runs on a guarded
       stack like every other thread, so an overflow is a panic naming "main"
       rather than corruption of whatever .bss sits underneath. Nothing on the
       old stack is needed again: the rest of boot is kmain_late, entered fresh. */
    uintptr_t main_top = kstack_alloc(KSTACK_MAIN_SIZE, 0, 0);
    if (!main_top) {
        klog("KSTACK", SEV_WARN, "no guarded stack for thread 0 - staying on the boot stack");
        kmain_late();
    }
    arch_run_on_stack(main_top, kmain_late);
}

/* Everything after the move to a guarded stack. Never returns. */
void kmain_late(void) {

    /* the platform's devices that need neither interrupts nor memory */
    platform_init();

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
    klog("KERNEL", SEV_OK, "CXK v" CXK_VERSION_STR " - higher-half kernel online");

    /* scheduler + timer (preemption available, enabled on demand). */
    sched_init();
    /* Thread 0 enters ring 3 too (the self-tests do), so it needs an esp0 stack
       of its own like any process. Without one, the TSS kept whichever esp0 the
       previous thread left, and thread 0's interrupts from ring 3 landed on
       another thread's kernel stack. Kernel-fatal rather than a WARN: thread 0
       runs ring-3 self-tests next, and the esp0 invariant (PROCESS_MODEL.md §8)
       says no thread enters ring 3 without its own. Failing a 8 KB allocation
       this early means the stack allocator itself is broken. */
    if (thread_alloc_kstack(0) != 0)
        kpanic("no esp0 stack for thread 0 (PROCESS_MODEL.md section 8)");
    klog("SCHED", SEV_OK, "online");

    timer_init();
    arch_irq_enable();                    /* enable interrupts: timer can fire */
    klog("TIMER", SEV_OK, "online (1000 Hz), interrupts enabled");

    /* calibrate the cycle counter against the tick so RTTs can be reported in
       microseconds rather than whole 1 ms ticks */
    timer_calibrate_tsc();
    klog_u32("TIMER", SEV_OK, "TSC calibrated, MHz: ", timer_tsc_mhz(), LOG_COLOR_VALUE, "");

    /* The pointer is clamped to the display so userspace gets an absolute
       position it can use directly; fb_width/fb_height return 0 in text mode,
       and mouse_init keeps its 640x480 default for a zero, so this is safe
       whether or not the framebuffer came up. Then the platform's own keyboard
       and mouse (PS/2 on a PC); USB ones arrive with the USB hosts. */
    mouse_init((int)fb_width(), (int)fb_height());
    platform_input_init();


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

    /* ACPI: enables the real shutdown (S5) and sleep paths.
       The status is dropped on purpose, and that is the documented behaviour
       rather than an oversight: a machine with no usable ACPI tables still
       boots and runs, it just loses the clean shutdown path, which power.c
       reports when something actually asks for one. Written as a cast so the
       audit of engineering §9 can tell "decided to ignore" from "forgot". */
    (void)acpi_init();

    /* Interrupt controllers, BEFORE any device driver. Two orderings matter:
       after acpi_init(), because the MADT is the only description of where the
       APICs are and how legacy IRQs reach them; and before the drivers, because
       a device cannot be given an MSI vector until the local APIC that would
       receive it is running.
       Declines to a working 8259 if anything is missing, so this cannot stop a
       machine booting - which is why the status is dropped on purpose: there
       is no failure here that is not already a working fallback. */
    (void)irq_controller_init();

    /* network: whichever NIC is on the bus (polled). netif_init() returns 0 if
       none is. It logs which driver bound, so this line no longer names one -
       it used to say "e1000 online" unconditionally, which became a lie the
       moment a second NIC driver existed. */
    if (netif_init()) klog("NET", SEV_OK, "online");
    else              klog("NET", SEV_WARN, "no NIC found - networking offline");

    /* storage: probe ATA + AHCI drives into the disk registry. AHCI is found via
       PCI; on a machine without one, ahci_init returns 0 harmlessly. */
    ata_init();
    /* Returns the number of ports it found, which is not what the boot log
       reports - that comes from disk_count(), the registry, so a port that
       was found but could not be registered is not counted twice. Dropped on
       purpose; ahci.c warns per port if the registry refuses one. */
    (void)ahci_init();

    /* USB AFTER the internal drives, and the order is load-bearing rather than
       stylistic. mount_cxfs() treats disk index 0 as the boot disk and never
       touches it, formatting index 1 as the data disk. Bringing USB up first
       gave a plugged-in flash drive index 0 and pushed the real boot disk to
       index 1 - so booting with a USB stick attached reformatted the disk the
       machine had just booted from. Internal storage must claim the low indices
       before anything removable can. */
    usb_init();
    klog_u32("STORAGE", SEV_OK, "online, disks: ", (uint32_t)disk_count(), LOG_COLOR_VALUE, "");

    /* Before the first write: which disks may be written at all. */
    int home = guard_disks();

    /* filesystem: mount CXFS on the data disk (read-only unless a dev build). */
    mount_cxfs(home);

    /* partitioned (XBPT) disk: ensure the SYSTEM partition is a mounted CXFS
       volume - formatting it and populating /System from the STAGE payload on
       first boot. No-ops if no XBPT disk is present. */
    int launch_exec = 0;
    {
        int ir = cxk_install_boot_disk(home);
        if      (ir == CXK_INSTALL_DONE)      klog("INSTALL", SEV_OK,   "first boot: /System created from staged payload");
        else if (ir == CXK_INSTALL_MOUNTED)   klog("INSTALL", SEV_OK,   "SYSTEM partition mounted");
        else if (ir == CXK_INSTALL_NO_SYSTEM) klog("INSTALL", SEV_INFO, "no XBPT system disk (skipped)");
        else klog_u32("INSTALL", SEV_WARN, "first-boot install issue: ", (uint32_t)ir, LOG_COLOR_VALUE, "");

        launch_exec = (ir == CXK_INSTALL_DONE || ir == CXK_INSTALL_MOUNTED);

        /* With / up, offer every other drive to /Drives. After the install
           so the tree exists, and before the self-tests so they see the same
           filesystem the shell will. */
        if (launch_exec) {
            int extra = cxk_mount_extra_volumes(cxfs_get_id());
            if (extra > 0)
                klog_u32("VOLUMES", SEV_OK, "extra volumes mounted: ",
                         (uint32_t)extra, LOG_COLOR_VALUE, "");
        }
    }

    /* The kernel's own limits, from /System/Config/kernel.xkco. As soon as the
       system volume can be read, and before the self-tests or the executive
       create a thread, so every thread is made under the configured limits. */
    if (launch_exec) kconfig_load();

    /* The crash area: a previous boot's panic goes to /System/crash.log, so
       after the system volume is up; before the self-tests, which write the
       area themselves. */
    crashlog_boot();

    /* run the kernel self-tests (ktest.c) first - they create + reap their own
       ring-3 threads, so let them finish before starting the executive. */
    ktest_run();

    /* `cxk verify-crashlog` armed the crash area: panic now, through the same
       path any panic takes, for the next boot to recover. After the
       self-tests, so this boot is an ordinary one up to here. */
    if (crashlog_test_requested()) {
        crashlog_test_interrupt();
        kpanic("deliberate panic: a crash-log test asked for one");
    }

    /* From here a user process that faults is saved to the crash area; the
       self-tests' faults, before this, are deliberate. */
    crashlog_faults_arm(1);

#ifdef CXK_DEV_UNSIGNED
    /* Said at every boot, in the log, just before anything unsigned could run:
       a development kernel must never be mistakable for a release one. */
    klog("KERNEL", SEV_WARN, "DEVELOPMENT KERNEL - unsigned programs will run");
    klog_child("built with DEV_UNSIGNED=ON; never ship this image");
#endif

    int xc_started = 0;
    /* capstone: start the signed executive as the root ring-3 process (its own
       address space + scheduler thread). It runs once we idle + yield below. */
    if (launch_exec) {
        klog("EXEC", SEV_INFO, "launching /System/Boot.xoex");
        int xc = cxk_launch_executive("/System/Boot.xoex");
        if (xc >= 0) {
            klog_u32("EXEC", SEV_INFO, "executive launched, pid ", (uint32_t)xc, LOG_COLOR_VALUE, "");   /* "started" comes from spawn.c, once it is loaded */
            xc_started = 1;
        }
        else {
            /* klog_u32 prints unsigned, so a bare -1 came out as 4294967295 -
               which reads like corruption rather than a negative error code.
               Negate for display and name the cause. */
            klog_u32("EXEC", SEV_ERR, "executive launch failed, code -",
                     (uint32_t)(-xc), LOG_COLOR_VALUE, "");
            klog_child(cxk_launch_strerror(xc));
        }
    }

    /* Two looks at the USB ports before the summary, 200 ms apart: a device
       EHCI handed to a companion at boot can have come back to EHCI since
       (the companion's start-up disconnects it), and is handed over again on
       the first look and found on the companion on the second. Without this
       the summary says no keyboard while one is about to come online. */
    usb_poll();
    timer_sleep(200);
    usb_poll();
    usb_summary();      /* last before the prompt: the rig has no serial port */
    klog("KERNEL", SEV_OK, "boot complete - idle");

    /* The screen is the shell's from here: kernel lines below SEV_ERR go to the
       serial port and the log ring, which the shell's `log` reads. Only once
       an executive is running - with none, the screen is all there is. */
    if (xc_started) klog_quiet();

    /* The log goes to the stick now and again every few seconds while it
       grows, so `cxk rig bootlog` reads it off a machine with no serial port:
       the boot at once, then the executive, the services and anything after. */
    int blk = usb_lock();
    int bl = crashlog_save_bootlog();
    usb_unlock(blk);
    if (bl == DISK_OK) klog("CRASHLOG", SEV_OK, "system log saved to the crash area (cxk rig bootlog)");
    else if (bl != DISK_ERR_NO_DEVICE)
        klog_u32("CRASHLOG", SEV_WARN, "system log not saved, disk error ", (uint32_t)-bl, LOG_COLOR_VALUE, "");
    uint32_t bl_at = timer_ticks();
    uint32_t usb_at = bl_at;

    for (;;) {
        yield();                       /* let the executive + its apps run */
        if (timer_since(bl_at) >= BOOTLOG_SAVE_MS && crashlog_bootlog_stale()) {
            int k = usb_lock();        /* the save writes with interrupts off: wait first */
            (void)crashlog_save_bootlog();
            usb_unlock(k);
            bl_at = timer_ticks();
        }
        if (timer_since(usb_at) >= USB_POLL_MS) {
            usb_poll();
            usb_at = timer_ticks();
        }
        arch_cpu_halt();
    }
}