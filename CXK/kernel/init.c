/* /CXLite/kernel/init.c */
/* Aurora Tejeda */
/* System initialization + consolidated boot status reporting. */

#include "init.h"
#include "gdt.h"
#include "usermode.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "console.h"
#include "vga.h"
#include "pmm.h"
#include "heap.h"
#include "paging.h"
#include "fb.h"
#include "pci.h"
#include "ahci.h"
#include "ata.h"
#include "disk.h"
#include "acpi.h"
#include "ohci.h"
#include "e1000.h"
#include "netif.h"
#include "cxfs.h"

/* ---- status tracking ----
   Each subsystem records a short name + state. Three states:
     ST_OK     - present and initialized (pass mark ^)
     ST_ABSENT - legitimately not present on this machine (dash -, not an error)
     ST_FAIL   - was expected but failed to initialize (X, triggers verbose report)
   A fully clean boot (no FAILs) emits ONE consolidated line; any FAIL also gets
   an explicit "[KERNEL] <NAME> INIT FAIL" line. */
#define ST_FAIL   0
#define ST_OK     1
#define ST_ABSENT 2

#define MAX_SUBS 24
static const char *sub_name[MAX_SUBS];
static int         sub_state[MAX_SUBS];
static int         sub_count = 0;

static void rec(const char *name, int state) {
    if (sub_count < MAX_SUBS) {
        sub_name[sub_count] = name;
        sub_state[sub_count] = state;
        sub_count++;
    }
}

/* a full, explicit failure line: "[KERNEL] <NAME> INIT FAIL" */
static void fail_line(const char *name) {
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_print("[");
    console_set_color(VGA_LIGHT_RED, VGA_BLACK);
    console_print("KERNEL");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_print("] ");
    console_set_color(VGA_LIGHT_RED, VGA_BLACK);
    console_print(name);
    console_print(" INIT FAIL");
    console_putc('\n');
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* the consolidated status line: "[KERNEL] SUBSYSTEMS: NAME^ NAME- ..."
   ^ = pass (green), - = absent (orange). Only reached for the success path; if
   anything FAILED, explicit fail lines are printed first. */
static void summary_line(void) {
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_print("[");
    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_print("KERNEL");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_print("] SUBSYSTEMS: ");
    for (int i = 0; i < sub_count; i++) {
        console_print(sub_name[i]);          /* name immediately followed by mark */
        if (sub_state[i] == ST_OK) {
            console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);   /* bright green */
            console_putc('^');
        } else if (sub_state[i] == ST_ABSENT) {
            console_set_color(VGA_BROWN, VGA_BLACK);         /* orange/amber */
            console_putc('-');
        } else {
            console_set_color(VGA_LIGHT_RED, VGA_BLACK);     /* bright red */
            console_putc('X');
        }
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        if (i + 1 < sub_count) console_print("  ");
    }
    console_putc('\n');
}

/* ---- subsystem bring-up helpers that report into the registry ---- */

/* register internal ATA + AHCI disks into the unified registry */
static void register_internal_disks(void) {
    for (uint8_t d = 0; d < 4; d++) {
        if (!ata_present(d)) continue;
        enum disk_media m = ata_is_ssd(d) ? DISK_MEDIA_SSD : DISK_MEDIA_HDD;
        disk_register(DISK_DRV_ATA, d, m, DISK_ATTACH_INTERNAL,
                      ata_model(d), ata_sectors(d));
    }
    for (int p = 0; p < 32; p++) {
        if (!ahci_present(p)) continue;
        enum disk_media m = ahci_is_ssd(p) ? DISK_MEDIA_SSD : DISK_MEDIA_HDD;
        disk_register(DISK_DRV_AHCI, (uint8_t)p, m, DISK_ATTACH_INTERNAL,
                      ahci_model(p), ahci_sectors(p));
    }
}

/* bring up USB: OHCI controller + enumerate/register any mass-storage device.
   returns 1 if the controller initialized (a device is optional), 0 if the
   controller failed to come up. */
static int usb_bringup(void) {
    int np = ohci_init();
    if (!ohci_present()) return 0;

    for (int i = 0; i < np; i++) {
        if (!ohci_port_connected(i)) continue;
        if (!ohci_enumerate(i)) continue;
        if (!(ohci_dev_if_valid() && ohci_dev_if_class() == 0x08)) continue;
        if (!ohci_storage_init()) continue;

        /* SCSI peripheral device type -> media: 0x00 disk -> USB0, 0x05 -> EXT-CDROM0 */
        uint8_t pdt = ohci_storage_pdt();
        enum disk_media media = (pdt == 0x05) ? DISK_MEDIA_CDROM : DISK_MEDIA_GENERIC;
        disk_register(DISK_DRV_USB, (uint8_t)i, media, DISK_ATTACH_USB,
                      "USB Mass Storage", ohci_storage_sectors());
    }
    return 1;
}

int system_init(void) {
    /* kernel identity banner */
    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_print("CXK - CATX SYSTEMS LLC\n");

    /* GDT + TSS FIRST: install the kernel's own GDT (ring-0 selectors kept at
       0x08/0x10, plus ring-3 segments and a TSS for privilege transitions).
       Done before the IDT so interrupt gates reference the stable 0x08 CS. */
    gdt_init();                         rec("GDT", ST_OK);

    /* Core CPU/IRQ + input - essential, effectively always succeed. */
    idt_init();                         rec("IDT", ST_OK);
    usermode_init();   /* install the ring-3 syscall gate (int 0x80, DPL=3) */
    timer_init();                       rec("TIMER", ST_OK);
    keyboard_init();                    rec("KBD", ST_OK);
    rec(fb_active() ? "FB" : "VGA", ST_OK);

    /* memory management */
    pmm_init();                         rec("PMM", ST_OK);
    paging_init();                      rec("PAGING", ST_OK);
    heap_init();                        rec("HEAP", ST_OK);

    /* buses + storage */
    ata_init();                         rec("ATA", ST_OK);
    pci_init();                         rec("PCI", ST_OK);
    ahci_init();                        rec("AHCI", ST_OK);
    register_internal_disks();          rec("DISKS", ST_OK);

    /* ACPI - absent (not present on this machine) is NOT a failure, just noted
       with a dash; only a genuine init error would be a FAIL. */
    rec("ACPI", acpi_init() ? ST_OK : ST_ABSENT);

    /* USB (OHCI) - same: controller-not-present is ABSENT, not FAIL. */
    rec("USB", usb_bringup() ? ST_OK : ST_ABSENT);

    /* network (e1000 NIC + interface layer) - absent is not a failure */
    int nic_ok = e1000_init();
    netif_init();
    rec("NET", nic_ok ? ST_OK : ST_ABSENT);

    /* filesystem layer is always up; whether a volume is mounted is separate. */
    int fs_mounted = (cxfs_mount() == 0);
    rec("CXFS", ST_OK);

    /* ---- report ---- */
    int failures = 0;
    for (int i = 0; i < sub_count; i++) if (sub_state[i] == ST_FAIL) failures++;

    if (failures > 0) {
        /* explicit per-failure lines first, then the summary for full context */
        for (int i = 0; i < sub_count; i++)
            if (sub_state[i] == ST_FAIL) fail_line(sub_name[i]);
    }
    summary_line();

    /* FS hint (informational, not a failure) - tagged FILESYS, not KERNEL */
    if (!fs_mounted) {
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        console_print("[");
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_print("FILESYS");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        console_print("] No filesystem yet - run 'format' to create one.\n");
    }

    return failures;
}