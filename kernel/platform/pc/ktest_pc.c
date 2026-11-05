// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/ktest_pc.c */
/* Aurora Tejeda / CATX Systems */
/* platform/ktest_platform.h on a PC: the boot self-tests that drive the PC's
   own devices, kept here because generic code may not name them. */

#include <stdint.h>
#include "platform/ktest_platform.h"
#include "i8042.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "logging.h"
#include "platform/pc/hwreport.h"
#include "diag/hwfmt.h"
#include "diag/crashrec.h"
#include "diag/crashlog.h"
#include "boot/cxbi.h"
#include "pci.h"
#include "acpi.h"
#include "disk.h"

/* ---- the PS/2 keyboard and mouse reach the kernel's input ----
 *
 * The 8042 will put a byte in its output buffer as though the keyboard (0xD2)
 * or the mouse (0xD3) had sent it, and raise the same interrupt. That drives
 * the whole path a key takes - the controller, IRQ 1 or 12 through whichever
 * interrupt controller is live, the decoder, the buffer - with nobody at the
 * keyboard. A machine with no 8042 (reads all ones) is untested, with a WARN. */
static int ps2_send(uint8_t cmd, uint8_t byte) {
    i8042_command(cmd);
    i8042_write(byte);
    /* Wait for the interrupt handler to take it. Real hardware latches the
       byte a little after the write, so an empty buffer means "taken" only
       once two ticks have passed. */
    uint32_t t0 = timer_ticks();
    while (timer_ticks() - t0 < 100)
        if (!(i8042_status() & I8042_STAT_OUTPUT_FULL) && timer_ticks() - t0 > 1) return 1;
    return 0;
}

int ktest_platform_input(void) {
    if (i8042_status() == 0xFF) {
        klog("KTEST", SEV_WARN, "PS/2 input untested: no 8042");
        return 1;
    }
    while (keyboard_getchar()) { }                         /* drain */

    /* 'a' pressed and released: one key, and the release none */
    if (!ps2_send(I8042_CMD_WRITE_KBD_OUT, 0x1E) || !ps2_send(I8042_CMD_WRITE_KBD_OUT, 0x9E)) {
        klog("KTEST", SEV_FAIL, "PS/2: the controller never handed over a keyboard byte");
        return 0;
    }
    char c1 = keyboard_getchar(), c2 = keyboard_getchar();
    if (c1 != 'a' || c2 != 0) {
        klog("KTEST", SEV_FAIL, "PS/2: a key sent through the 8042 did not arrive as 'a'");
        return 0;
    }

    if (!mouse_present()) {
        klog("KTEST", SEV_WARN, "PS/2 mouse untested: none attached");
        return 1;
    }
    /* Right 5 and up 3, then left 5 and down 3, which uses the 9-bit signs.
       PS/2 Y is positive-up and the screen's is positive-down, so "up 3" is
       3 rows nearer the top. The cursor starts centred, clear of the clamp. */
    int32_t x0 = mouse_x(), y0 = mouse_y();
    static const uint8_t pkt[2][3] = { { 0x08, 5, 3 }, { 0x38, 0xFB, 0xFD } };
    static const int32_t want_dx[2] = { 5, 0 }, want_dy[2] = { -3, 0 };
    for (int p = 0; p < 2; p++) {
        uint32_t seq0 = mouse_seq();
        for (int i = 0; i < 3; i++)
            if (!ps2_send(I8042_CMD_WRITE_AUX_OUT, pkt[p][i])) {
                klog("KTEST", SEV_FAIL, "PS/2: the controller never handed over a mouse byte");
                return 0;
            }
        if (mouse_seq() - seq0 != 1) {
            klog("KTEST", SEV_FAIL, "PS/2: a mouse packet was not one movement");
            return 0;
        }
        if (mouse_x() != x0 + want_dx[p] || mouse_y() != y0 + want_dy[p]) {
            klog("KTEST", SEV_FAIL, "PS/2: a mouse packet moved the pointer wrongly");
            return 0;
        }
    }
    return 1;
}

/* ---- the hardware report (hwreport.h) ----
 *
 * The form's own rules are host tests (test_hwfmt.c). These are what only a
 * boot has: the report this boot built checks, has each section, counts what
 * the kernel itself found (the loader's map, the PCI functions, a CPU in the
 * MADT), went to the crash area byte for byte, and its two measurements of
 * the TSC agree. The first FAIL line names the reason, short. */
static char hw_back[XCRL_HWREPORT_MAX];

static int hw_fail(const char *why) {
    klog("KTEST", SEV_FAIL, why);
    return 0;
}

/* Lines "@hw<TAB><section><TAB>" (or ending there) whose text also holds
   `has` (0 for any). */
static uint32_t hw_count(const char *t, uint32_t len, const char *section, const char *has) {
    uint32_t n = 0, sl = 0, hl = 0;
    while (section[sl]) sl++;
    while (has && has[hl]) hl++;
    for (uint32_t at = 0; at < len; ) {
        uint32_t e = at;
        while (e < len && t[e] != '\n') e++;
        int ok = e - at >= 4 + sl && (e - at == 4 + sl || t[at + 4 + sl] == '\t');
        for (uint32_t i = 0; ok && i < sl; i++) if (t[at + 4 + i] != section[i]) ok = 0;
        if (ok && hl) {
            ok = 0;
            for (uint32_t i = at; !ok && i + hl <= e; i++) {
                uint32_t k = 0;
                while (k < hl && t[i + k] == has[k]) k++;
                ok = k == hl;
            }
        }
        n += (uint32_t)ok;
        at = e + 1;
    }
    return n;
}

int ktest_platform_hw_report(void) {
    uint32_t len = 0;
    const char *t = hwreport_text(&len);
    if (!t) return hw_fail("hwr: no report was built");
    if (hw_check(t, len) != HWR_OK) return hw_fail("hwr: the report fails its own check");

    if (hw_count(t, len, "boot", 0) != 1 || hw_count(t, len, "cpu", 0) != 1
        || hw_count(t, len, "hypervisor", 0) != 1 || hw_count(t, len, "cpuid", 0) < 2
        || hw_count(t, len, "memsum", 0) != 1 || hw_count(t, len, "pcisum", 0) != 1
        || hw_count(t, len, "apic", 0) != 1 || hw_count(t, len, "clock", 0) != 10)
        return hw_fail("hwr: a section is missing or doubled");
    if (hw_count(t, len, "mem", 0) != cxbi_mmap_count())
        return hw_fail("hwr: mem lines are not the loader's map");
    if (hw_count(t, len, "pci", 0) != pci_device_count())
        return hw_fail("hwr: pci lines are not the bus's functions");
    if (cxbi()->acpi_rsdp) {
        if (hw_count(t, len, "acpi", "\tacpi\trsdp\t") != 1)  /* not "table rsdp RSDT" */
            return hw_fail("hwr: no RSDP line");
        if (hw_count(t, len, "acpi", "\tFACP\t") == 0)
            return hw_fail("hwr: no FADT line");
    }
    if (hw_count(t, len, "apic", "\tapic\t1\t")) {      /* the report: I/O APIC routing */
        if (hw_count(t, len, "acpi", "\tAPIC\t") == 0 || hw_count(t, len, "madt", "head") != 1)
            return hw_fail("hwr: the APIC is up but no MADT");
        if (hw_count(t, len, "madt", "\tcpu\t") + hw_count(t, len, "madt", "\tx2cpu\t") == 0)
            return hw_fail("hwr: the MADT names no CPU");
    }

    uint8_t disk; uint64_t lba, secs;
    if (crashlog_area(&disk, &lba, &secs) == 0) {
        if (hwreport_saved() != DISK_OK) return hw_fail("hwr: not saved to the crash area");
        uint32_t back = 0;
        if (crashlog_hwreport_read(hw_back, sizeof hw_back, &back) != DISK_OK)
            return hw_fail("hwr: the saved record does not read back");
        if (back != len) return hw_fail("hwr: the saved record's length differs");
        for (uint32_t i = 0; i < len; i++)
            if (hw_back[i] != t[i]) return hw_fail("hwr: the saved record differs");
    }

    /* A PC has a PIT: no rate from its polled count means the measurement
       failed, and a TSC taken over the tick's interrupts is a host's
       delivery rate, not the counter's (1.8c). */
    if (timer_tsc_hz() && !timer_tsc_pit_hz())
        return hw_fail("hwr: the PIT's count measured no TSC rate");
    if (timer_tsc_hz() && timer_tsc_by()[0] == 't')
        return hw_fail("hwr: the TSC was measured by the tick");
    uint32_t pm;
    uint64_t by_pm = hwreport_tsc_pm_hz(), by_clock = timer_tsc_hz();
    if (acpi_pm_timer_read(&pm) == 0 && by_clock) {
        if (!by_pm) return hw_fail("hwr: the PM timer measured no TSC rate");
        uint64_t d = by_pm > by_clock ? by_pm - by_clock : by_clock - by_pm;
        if (d > by_clock / 100) {
            klog_u32("KTEST", SEV_FAIL, "hwr: TSC by clock and by PM timer differ, MHz ",
                     (uint32_t)(by_clock / 1000000u), LOG_COLOR_VALUE, " the clock's");
            klog_child_u32("by the PM timer, MHz: ", (uint32_t)(by_pm / 1000000u), LOG_COLOR_VALUE, "");
            return 0;
        }
    }
    return 1;
}
