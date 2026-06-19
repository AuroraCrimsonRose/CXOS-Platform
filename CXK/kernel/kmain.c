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
#include "heap.h"
#include "console.h"
#include "color.h"
#include "fb.h"
#include "resolution.h"
#include "format.h"
#include "sched.h"
#include "timer.h"
#include "usermode.h"
#include "ata.h"
#include "ahci.h"
#include "disk.h"
#include "cxfs.h"
#include "pci.h"
#include "ktest.h"
#include "klogo.h"
#include "speaker.h"

#define CXLOGO_WIDTH 600
#define CXLOGO_HEIGHT 600

/* Verbose-only boot logging. When CXK_VERBOSE_BOOT is 0 (default) these are
   no-ops, so a normal boot is quiet; set the flag for a full progress log. */
#if CXK_VERBOSE_BOOT
static void vlog(const char *msg) { console_boot(msg); }
static void vlog_u32(const char *label, uint32_t v, uint8_t attr) {
    console_field_u32(label, v, attr); console_newline();
}
#else
static void vlog(const char *msg) { (void)msg; }
static void vlog_u32(const char *label, uint32_t v, uint8_t attr) {
    (void)label; (void)v; (void)attr;
}
#endif

/* Mount CXFS on the data disk (first non-boot disk). Read-only by default:
   mounts an existing filesystem if present and NEVER formats, so this can't
   wipe a non-CXFS disk on real hardware. A dev build (CXK_ALLOW_DISK_WRITE)
   formats a scratch disk if it has no filesystem. The boot disk (index 0) is
   never touched. */
static void mount_cxfs(void) {
    const struct disk *fsdisk = 0;
    if (disk_count() >= 2) fsdisk = disk_get(1);   /* first non-boot disk */
    if (!fsdisk) { vlog("CXFS: no data disk present"); return; }

    cxfs_set_id(fsdisk->id);
    if (cxfs_mount() == 0) {
        vlog("CXFS mounted (existing filesystem)");
    } else {
#if CXK_ALLOW_DISK_WRITE
        vlog("CXFS: no filesystem - formatting (dev build)");
        if (cxfs_format() == 0 && cxfs_mount() == 0)
            vlog("CXFS formatted + mounted");
        else
            console_err("CXFS: format/mount FAILED");   /* failures always show */
#else
        vlog("CXFS: no filesystem present (read-only, untouched)");
#endif
    }
    if (cxfs_is_mounted())
        vlog_u32("       CXFS free blocks: ", cxfs_free_blocks(),
                 VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK));
}

#if CXK_ENABLE_FB
/* Framebuffer boot splash - drawn once after fb_init succeeds, to PROVE the LFB
   maps and renders: the color bars exercise rgb packing + rect fill, the frame
   checks the edge margins, and the text exercises 8x16 glyph rendering. The
   text console still targets VGA text here (invisible in graphics mode), so
   this splash is the only thing on screen until CP3 routes the console onto the
   framebuffer. */
static void fb_boot_splash(void) {
    uint32_t w = fb_width(), h = fb_height();
    uint32_t bg = fb_rgb(0x10, 0x10, 0x18);
    fb_clear(bg);

    static const uint8_t bar[8][3] = {
        {255,0,0}, {0,255,0}, {0,0,255}, {255,255,0},
        {0,255,255}, {255,0,255}, {255,255,255}, {120,120,120}
    };
    uint32_t bw = w / 8, barh = h / 6;
    for (uint32_t i = 0; i < 8; i++)
        fb_fill_rect(i * bw, 0, bw, barh, fb_rgb(bar[i][0], bar[i][1], bar[i][2]));

    /* white frame: confirms we reach every edge with no off-by-one clipping */
    uint32_t white = fb_rgb(255, 255, 255);
    fb_draw_line(0, 0, (int)w - 1, 0, white);
    fb_draw_line(0, (int)h - 1, (int)w - 1, (int)h - 1, white);
    fb_draw_line(0, 0, 0, (int)h - 1, white);
    fb_draw_line((int)w - 1, 0, (int)w - 1, (int)h - 1, white);

    uint32_t fg = fb_rgb(0xE0, 0xE0, 0xE0);
    uint32_t y = barh + 12;
    fb_draw_string(16, y, "CXK v5 - framebuffer online", fg, bg);
    y += fb_font_height() + 4;

    /* "WxH BPPbpp (NAME)" via the formatter + resolution catalog */
    char line[64];
    size_t n = 0;
    n += fmt_u32(line + n, w);        line[n++] = 'x';
    n += fmt_u32(line + n, h);        line[n++] = ' ';
    n += fmt_u32(line + n, fb_bpp()); line[n++] = 'b'; line[n++] = 'p'; line[n++] = 'p';
    line[n++] = ' '; line[n++] = '(';
    const char *nm = res_name(w, h);
    while (*nm) line[n++] = *nm++;
    line[n++] = ')';
    line[n] = 0;
    fb_draw_string(16, y, line, fg, bg);
}
#endif

void kmain(void) {
    /* console first, so everything after it logs cleanly. */
    console_init();
    console_kernel("CXK v5 - higher-half kernel online");

    /* CPU core: segments + interrupts. */
    gdt_init();
    vlog("GDT + TSS installed");
    idt_init();
    vlog("IDT installed");

    /* memory: physical frames -> virtual mappings -> heap. */
    pmm_init();
    vlog("PMM online");
    vlog_u32("       total pages: ", pmm_total_pages(), VGA_ATTR(VGA_WHITE, VGA_BLACK));
    vlog_u32("       free pages:  ", pmm_free_count(),  VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK));

    paging_init();
    vlog("Paging online (recursive page directory)");

    heap_init();
    vlog("Heap online (kmalloc/kfree)");

    /* initalize speakers */
    speaker_init();

#if CXK_ENABLE_FB
    if (fb_init() == 0)
    {
        fb_clear(fb_rgb(0,0,0));

        klogo_draw(
            (fb_width()  - CXLOGO_WIDTH)  / 2,
            (fb_height() - CXLOGO_HEIGHT) / 2
        );
        speaker_boot_chime();

        timer_sleep(1000);

        fb_boot_splash();
    }
#endif

    /* scheduler + timer (preemption available, enabled on demand). */
    sched_init();
    vlog("Scheduler online");

    timer_init();
    __asm__ __volatile__("sti");          /* enable interrupts: timer can fire */
    vlog("Timer online (PIT @ 1000 Hz), interrupts enabled");


    /* user mode: syscall gate + ring-3 fault handler. */
    usermode_init();
    usermode_register_fault_handler();
    vlog("Usermode online (syscall gate int 0x80)");

    /* PCI bus: enumerate devices so storage/USB/NIC drivers can find their
       controllers. Must come before AHCI (which is discovered via PCI). */
    pci_init();
    vlog_u32("PCI enumerated: devices: ", (uint32_t)pci_device_count(),
             VGA_ATTR(VGA_WHITE, VGA_BLACK));

    /* storage: probe ATA + AHCI drives into the disk registry. AHCI is found via
       PCI; on a machine without one, ahci_init returns 0 harmlessly. */
    ata_init();
    ahci_init();
    vlog_u32("Storage online: disks: ", (uint32_t)disk_count(),
             VGA_ATTR(VGA_WHITE, VGA_BLACK));

    /* filesystem: mount CXFS on the data disk (read-only unless a dev build). */
    mount_cxfs();

    /* run the kernel self-tests (ktest.c). */
    ktest_run();

    console_kernel("boot complete - idle");
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}