// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/hwreport.c */
/* Aurora Tejeda / CATX Systems */
/* The hardware report. See hwreport.h, and diag/hwfmt.h for the form. */

#include "platform/platform.h"
#include "platform/pc/hwreport.h"
#include "diag/hwfmt.h"
#include "diag/crashrec.h"
#include "diag/crashlog.h"
#include "boot/cxbi.h"
#include "arch/paging.h"
#include "arch/cpu_ops.h"
#include "arch/evtimer.h"
#include "arch/hwreport.h"
#include "acpi.h"
#include "pci.h"
#include "timer.h"
#include "serial.h"
#include "disk.h"
#include "logging.h"

/* Whole sectors, so the save writes straight from it (zero past the end). */
static char     report[XCRL_HWREPORT_MAX];
static uint32_t report_len;
static uint64_t tsc_pm_hz;
static int      saved = DISK_ERR_NO_DEVICE;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static int sig_is(const uint8_t *t, const char *s) {
    return t[0] == s[0] && t[1] == s[1] && t[2] == s[2] && t[3] == s[3];
}

/* ---- boot ---- */

static void boot_line(struct hw_out *o) {
    const struct cxk_boot_info *bi = cxbi();
    hw_line(o, "boot");
    hw_s(o, bi->firmware == CXBI_FW_BIOS ? "bios" : bi->firmware == CXBI_FW_UEFI ? "uefi" : "unknown");
    hw_x(o, bi->flags, 8);
    hw_x(o, bi->smbios, 16);
    hw_done(o);
}

/* ---- memory ---- */

static const char *mem_type(uint32_t t) {
    switch (t) {
    case CXBI_MEM_USABLE:       return "usable";
    case CXBI_MEM_RESERVED:     return "reserved";
    case CXBI_MEM_ACPI_RECLAIM: return "acpi-reclaim";
    case CXBI_MEM_ACPI_NVS:     return "acpi-nvs";
    case CXBI_MEM_BAD:          return "bad";
    case CXBI_MEM_LOADER:       return "loader";
    case CXBI_MEM_FRAMEBUFFER:  return "framebuffer";
    default:                    return "unknown";
    }
}

static void mem_lines(struct hw_out *o) {
    uint64_t usable = 0;
    uint32_t n = cxbi_mmap_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (!e) continue;
        if (e->type == CXBI_MEM_USABLE) usable += e->length;
        hw_line(o, "mem");
        hw_x(o, e->base, 16);
        hw_x(o, e->length, 16);
        hw_s(o, mem_type(e->type));
        hw_x(o, e->attr, 8);
        hw_done(o);
    }
    hw_line(o, "memsum");
    hw_u(o, n);
    hw_u(o, cxbi_mmap_dropped());
    hw_x(o, usable, 16);
    hw_done(o);
}

/* ---- ACPI ---- */

#define SDT_HEADER     36u
#define TABLE_MAP_MAX  (16u << 20)   /* a longer "table" is not mapped */
#define ROOT_ENTRIES   256u

static const uint8_t *fadt;           /* the first FACP found, mapped whole */
static uint32_t       fadt_len;
static const uint8_t *madt;           /* the first APIC, mapped whole */
static uint32_t       madt_len;

static const uint8_t *map(uint64_t phys, uint32_t len) {
    return (const uint8_t *)paging_map_phys((paddr_t)phys, len, VM_WRITE);
}

/* Map a table whole when its length is believable; its line either way.
   The table, or 0 when only its header (or nothing) could be read. */
static const uint8_t *table(struct hw_out *o, const char *via, uint64_t phys) {
    const uint8_t *h = phys ? map(phys, SDT_HEADER) : 0;
    if (!h) { hw_table(o, via, phys, 0, 0); return 0; }
    uint32_t len = rd32(h + 4);
    int whole = len >= SDT_HEADER && len <= TABLE_MAP_MAX && map(phys, len) == h;
    hw_table(o, via, phys, h, whole);
    return whole ? h : 0;
}

static void root_walk(struct hw_out *o, const char *via, const uint8_t *root, uint32_t esz) {
    uint32_t len = rd32(root + 4);
    uint32_t n = (len - SDT_HEADER) / esz;
    if (n > ROOT_ENTRIES) n = ROOT_ENTRIES;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = root + SDT_HEADER + i * esz;
        uint64_t phys = esz == 8 ? rd64(p) : rd32(p);
        const uint8_t *t = table(o, via, phys);
        if (!t) continue;
        if (!fadt && sig_is(t, "FACP")) { fadt = t; fadt_len = rd32(t + 4); }
        if (!madt && sig_is(t, "APIC")) { madt = t; madt_len = rd32(t + 4); }
    }
}

/* The FADT's two pointers, each in its 32-bit field and, from ACPI 2.0, a
   64-bit one; the 64-bit one gets a line of its own only when it differs. */
static void fadt_pointer(struct hw_out *o, uint32_t off32, uint32_t off64) {
    uint64_t a = rd32(fadt + off32);
    uint64_t b = fadt_len >= off64 + 8 ? rd64(fadt + off64) : 0;
    if (a) (void)table(o, "fadt", a);
    if (b && b != a) (void)table(o, "xfadt", b);
}

static void acpi_lines(struct hw_out *o) {
    fadt = madt = 0;
    uint64_t at = cxbi()->acpi_rsdp;
    const uint8_t *r = at ? map(at, 36) : 0;
    if (!r || r[0] != 'R' || r[1] != 'S' || r[2] != 'D' || r[3] != ' ') {
        hw_line(o, "acpi");
        hw_s(o, at ? "rsdp-bad" : "none");
        hw_x(o, at, 16);
        hw_done(o);
        return;
    }
    hw_rsdp(o, at, r);

    const uint8_t *rsdt = table(o, "rsdp", rd32(r + 16));
    if (rsdt && rd32(rsdt + 4) >= SDT_HEADER) root_walk(o, "rsdt", rsdt, 4);
    if (r[15] >= 2) {
        uint64_t x = rd64(r + 24);
        const uint8_t *xsdt = x ? table(o, "rsdp", x) : 0;
        if (xsdt && rd32(xsdt + 4) >= SDT_HEADER) root_walk(o, "xsdt", xsdt, 8);
    }
    if (fadt) {
        fadt_pointer(o, 40, 140);   /* DSDT, X_DSDT */
        fadt_pointer(o, 36, 132);   /* FACS, X_FIRMWARE_CTRL */
    }
    if (madt) hw_madt(o, madt, madt_len);
}

/* ---- PCI ---- */

static void pci_lines(struct hw_out *o) {
    static const char hex[] = "0123456789abcdef";
    unsigned n = pci_device_count();
    for (unsigned i = 0; i < n; i++) {
        const struct pci_device *d = pci_get(i);
        if (!d) continue;
        char bdf[8] = { hex[d->bus >> 4], hex[d->bus & 15], ':', hex[d->slot >> 4], hex[d->slot & 15],
                        '.', hex[d->func & 7], 0 };
        hw_line(o, "pci");
        hw_s(o, bdf);
        hw_x(o, d->vendor_id, 4);
        hw_x(o, d->device_id, 4);
        hw_x(o, d->class_code, 2);
        hw_x(o, d->subclass, 2);
        hw_x(o, d->prog_if, 2);
        hw_x(o, d->header_type, 2);
        if ((d->header_type & 0x7F) == 0) {
            uint32_t ss = pci_config_read32(d->bus, d->slot, d->func, 0x2C);
            hw_x(o, ss & 0xFFFF, 4);
            hw_x(o, ss >> 16, 4);
        } else {
            hw_s(o, "-");
            hw_s(o, "-");
        }
        uint32_t irq = pci_config_read32(d->bus, d->slot, d->func, 0x3C);
        hw_x(o, irq & 0xFF, 2);
        hw_x(o, (irq >> 8) & 0xFF, 2);
        for (int b = 0; b < 6; b++) hw_x(o, d->bar[b], 8);
        hw_done(o);
    }
    hw_line(o, "pcisum");
    hw_u(o, n);
    hw_u(o, PCI_MAX_DEVICES);
    hw_done(o);
}

/* ---- clocks ---- */

#define PM_WINDOW      (ACPI_PM_TIMER_HZ / 20u)   /* 50 ms of it */
#define PM_GIVE_UP_NS  1000000000ull

/* The TSC against the PM timer over 50 ms, read 24 bits wide (all a PM timer
   is required to have). Each sample takes the PM timer and the TSC with
   interrupts off, so one landing between them does not count. 0 when there
   is no PM timer, or it did not move within a second. */
static uint64_t measure_tsc_pm(void) {
    uint32_t a, b;
    if (acpi_pm_timer_read(&a) != 0) return 0;
    uint64_t give_up = timer_now_ns() + PM_GIVE_UP_NS;

    uint64_t c0;
    for (;;) {                                   /* from an edge of the PM count */
        uint32_t f = arch_irq_save();
        (void)acpi_pm_timer_read(&b);
        c0 = arch_cycles64();
        arch_irq_restore(f);
        if (b != a) break;
        if (timer_now_ns() > give_up) return 0;
    }
    uint32_t start = b, ticks;
    uint64_t c1;
    for (;;) {
        uint32_t f = arch_irq_save();
        (void)acpi_pm_timer_read(&b);
        c1 = arch_cycles64();
        arch_irq_restore(f);
        ticks = (b - start) & 0xFFFFFFu;
        if (ticks >= PM_WINDOW) break;
        if (timer_now_ns() > give_up) return 0;
    }
    return (c1 - c0) * ACPI_PM_TIMER_HZ / ticks;
}

static void clock_lines(struct hw_out *o) {
    hw_line(o, "clock"); hw_s(o, "source"); hw_s(o, timer_clock_is_tsc() ? "tsc" : "tick"); hw_done(o);
    hw_line(o, "clock"); hw_s(o, "invariant"); hw_u(o, (uint64_t)arch_cycles_constant()); hw_done(o);
    hw_line(o, "clock"); hw_s(o, "tsc-hz"); hw_u(o, timer_tsc_hz()); hw_done(o);
    hw_line(o, "clock"); hw_s(o, "tsc-by"); hw_s(o, timer_tsc_by()); hw_done(o);
    hw_line(o, "clock"); hw_s(o, "tsc-pit-hz");
    if (timer_tsc_pit_hz()) hw_u(o, timer_tsc_pit_hz()); else hw_s(o, "-");
    hw_done(o);
    hw_line(o, "clock"); hw_s(o, "tick-hz"); hw_u(o, TIMER_TICK_HZ); hw_done(o);
    hw_line(o, "clock"); hw_s(o, "tick-irq-hz");
    if (timer_tick_irq_hz()) hw_u(o, timer_tick_irq_hz()); else hw_s(o, "-");
    hw_done(o);
    hw_line(o, "clock"); hw_s(o, "evtimer-hz"); hw_u(o, timer_evtimer_hz()); hw_done(o);

    hw_line(o, "clock");
    hw_s(o, "pmtimer");
    if (fadt && fadt_len >= 116 && rd32(fadt + 76)) {
        hw_x(o, rd32(fadt + 76), 4);
        hw_u(o, (rd32(fadt + 112) & (1u << 8)) ? 32 : 24);   /* TMR_VAL_EXT */
    } else {
        hw_s(o, "-");
    }
    hw_done(o);

    tsc_pm_hz = measure_tsc_pm();
    hw_line(o, "clock");
    hw_s(o, "tsc-pm-hz");
    if (tsc_pm_hz) hw_u(o, tsc_pm_hz); else hw_s(o, "-");
    hw_done(o);
}

/* ---- the report ---- */

static void print_serial(void) {
    for (uint32_t at = 0; at < report_len; ) {
        uint32_t e = at;
        while (e < report_len && report[e] != '\n') e++;
        /* A line at a time with interrupts off, as the @ktest lines: a klog
           cannot split one, and a reader matches "@hw" anywhere in a line. */
        uint32_t f = arch_irq_save();
        serial_puts("\r\n");
        for (uint32_t i = at; i < e; i++) serial_putc(report[i]);
        serial_puts("\r\n");
        arch_irq_restore(f);
        at = e + 1;
    }
}

void platform_hw_report(void) {
    struct hw_out o;
    hw_open(&o, report, sizeof report);
    boot_line(&o);
    arch_hw_report(&o);
    mem_lines(&o);
    acpi_lines(&o);
    pci_lines(&o);
    clock_lines(&o);
    report_len = hw_close(&o);
    for (uint32_t i = report_len; i < sizeof report; i++) report[i] = 0;

    print_serial();
    saved = crashlog_hwreport_write(report, report_len);
    klog_u32("HW", saved == DISK_OK ? SEV_OK : SEV_WARN, "report: lines ", o.lines + 1,
             LOG_COLOR_VALUE, saved == DISK_OK ? ", saved to the crash area" : ", NOT saved");
    if (o.over) klog("HW", SEV_WARN, "report cut short: it ran out of room");
}

const char *hwreport_text(uint32_t *len) {
    *len = report_len;
    return report_len ? report : 0;
}

uint64_t hwreport_tsc_pm_hz(void) { return tsc_pm_hz; }
int      hwreport_saved(void)     { return saved; }
