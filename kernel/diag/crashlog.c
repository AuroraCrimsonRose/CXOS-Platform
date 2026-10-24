// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/crashlog.c */
/* Aurora Tejeda / CATX Systems */
/* The crash log. See crashlog.h, and crashrec.h for the record. */

#include "diag/crashlog.h"
#include "diag/crashrec.h"
#include "disk.h"
#include "partition.h"
#include "cxfs.h"
#include "logging.h"
#include "syslog.h"
#include "config.h"
#include "format.h"
#include "arch/cpu_ops.h"

#define CRASH_FILE "/System/crash.log"
#define FAULT_FILE "/System/faults.log"

/* Everything here is static: the panic path may not allocate. `text` is the
   panic's buffer, and at boot the recovered record's, before any panic can
   happen. */
static char     text[XCRL_TEXT_MAX];
static uint32_t text_len;
static uint8_t  hdr[XCRL_SECTOR];

static int      have_area;
static uint8_t  area_disk;
static uint64_t area_lba;
static uint64_t area_sectors;

static int      test_requested;
static int      saving;
static int      recovered;           /* this boot copied a record to the file */
static uint32_t recovered_len, recovered_crc;

static void clear_text(void) {
    for (uint32_t i = 0; i < XCRL_TEXT_MAX; i++) text[i] = 0;
    text_len = 0;
}

/* The first disk with a CRASH partition big enough for a record, among the
   disks this boot may write (disk_restrict_writes): a crash area found on any
   other disk would have every record refused, and a panic would lose its
   record without a word. */
static void find_area(void) {
#if !CXK_ALLOW_DISK_WRITE
    /* config.h: this build writes to no disk, the crash area included. */
    klog("CRASHLOG", SEV_INFO, "crash log off: this build writes to no disk");
    return;
#endif
    int home = disk_write_home();
    for (unsigned i = 0; i < disk_count(); i++) {
        const struct disk *d = disk_get(i);
        if (!d) continue;
        if (home != DISK_WRITES_ANY && d->id != home) continue;
        struct partition p;
        if (part_find_type(d->id, PART_TYPE_CXCRASH, &p) != 0) continue;
        if (p.sectors < XCRL_SECTORS) {
            klog("CRASHLOG", SEV_WARN, "crash area too small, not used");
            continue;
        }
        have_area = 1;
        area_disk = d->id;
        area_lba = p.start_lba;
        area_sectors = p.sectors;
        return;
    }
}

int crashlog_write_record(uint16_t kind, const char *t, uint32_t len) {
    if (!have_area) return DISK_ERR_NO_DEVICE;
    if (len > XCRL_TEXT_MAX) len = XCRL_TEXT_MAX;
    uint32_t secs = (len + XCRL_SECTOR - 1) / XCRL_SECTOR;
    int r;
    if (secs) {
        r = disk_write(area_disk, area_lba + 1, secs, t);
        if (r != DISK_OK) return r;
    }
    xcrl_seal(hdr, kind, t, len);
    r = disk_write(area_disk, area_lba, 1, hdr);
    if (r != DISK_OK) return r;
    return disk_flush(area_disk);
}

int crashlog_clear(void) {
    if (!have_area) return DISK_ERR_NO_DEVICE;
    for (uint32_t i = 0; i < XCRL_SECTOR; i++) hdr[i] = 0;
    int r = disk_write(area_disk, area_lba, 1, hdr);
    if (r != DISK_OK) return r;
    return disk_flush(area_disk);
}

/* The panic's reason: the second line of the record. Never the first, which is
   the panic banner - a boot that prints it reads as a panic to anything
   watching the serial log, `cxk verify-boot` included. */
static void log_reason(void) {
    uint32_t i = 0;
    while (i < text_len && text[i] != '\n') i++;
    i++;
    char line[72];
    uint32_t n = 0;
    while (i < text_len && text[i] != '\n' && n < sizeof line - 1) line[n++] = text[i++];
    line[n] = 0;
    if (n) klog_child(line);
}

/* A previous boot's record into /System/crash.log, or a user-fault record into
   /System/faults.log. The area is cleared only once the file is written, so a
   failure here loses nothing. */
static void recover(uint16_t kind, uint32_t len) {
    const int fault = kind == XCRL_KIND_FAULT;
    const char *path = fault ? FAULT_FILE : CRASH_FILE;
    const char *name = fault ? "faults.log" : "crash.log";
    uint32_t secs = (len + XCRL_SECTOR - 1) / XCRL_SECTOR;
    if (secs && disk_read(area_disk, area_lba + 1, secs, text) != DISK_OK) {
        klog("CRASHLOG", SEV_WARN, "previous crash log unreadable, left in place");
        clear_text();
        return;
    }
    if (xcrl_check_text(hdr, text, len) != XCRL_VALID) {
        klog("CRASHLOG", SEV_WARN, "previous crash log torn (CRC), discarded");
        clear_text();
        (void)crashlog_clear();
        return;
    }
    text_len = len;
    if (!cxfs_is_mounted()) {
        klog("CRASHLOG", SEV_WARN, "previous crash log kept: no system volume to save it to");
        clear_text();
        return;
    }
    int id = cxfs_resolve(path, 0);
    if (id < 0) {
        int dir = cxfs_resolve("/System", 0);
        if (dir >= 0) id = cxfs_create_entry((uint32_t)dir, name, CXFS_TYPE_FILE);
    }
    if (id < 0 || cxfs_write_file((uint32_t)id, text, len) != 0) {
        klog("CRASHLOG", SEV_WARN, fault ? "previous user-fault log kept: " FAULT_FILE " not written"
                                         : "previous crash log kept: " CRASH_FILE " not written");
        clear_text();
        return;
    }
    klog("CRASHLOG", SEV_WARN, fault ? "recovered the previous boot's user faults to " FAULT_FILE
                                     : "recovered the previous boot's crash log to " CRASH_FILE);
    log_reason();
    if (fault) {                         /* not a crash: crashlog_recovered stays 0 */
        clear_text();
        if (crashlog_clear() != DISK_OK)
            klog("CRASHLOG", SEV_WARN, "crash area not cleared; it will be recovered again");
        return;
    }
    recovered = 1;
    recovered_len = len;
    recovered_crc = xcrl_crc32(0, text, len);
    clear_text();
    if (crashlog_clear() != DISK_OK)
        klog("CRASHLOG", SEV_WARN, "crash area not cleared; it will be recovered again");
}

void crashlog_boot(void) {
    find_area();
    if (!have_area) {
        klog("CRASHLOG", SEV_INFO, "no crash area: a panic will not be saved");
        return;
    }
    if (disk_read(area_disk, area_lba, 1, hdr) != DISK_OK) {
        klog("CRASHLOG", SEV_WARN, "crash area unreadable");
        return;
    }
    uint16_t kind = 0;
    uint32_t len = 0;
    int st = xcrl_check_header(hdr, &kind, &len);
    if (st == XCRL_VALID && kind == XCRL_KIND_REQUEST) {
        /* Cleared before anything else, so the deliberate panic happens once
           and the next boot is an ordinary one. */
        if (xcrl_check_text(hdr, 0, 0) == XCRL_VALID && crashlog_clear() == DISK_OK) {
            test_requested = 1;
            klog("CRASHLOG", SEV_WARN, "a crash-log test asked this boot to panic");
        }
    } else if (st == XCRL_VALID && kind == XCRL_KIND_BOOTLOG) {
        /* Never written here: its text would not fit the recovery buffer. */
        klog("CRASHLOG", SEV_WARN, "crash area held a boot log at the record's place; cleared");
        (void)crashlog_clear();
    } else if (st == XCRL_VALID) {
        recover(kind, len);
    } else if (st != XCRL_EMPTY) {
        klog_u32("CRASHLOG", SEV_WARN, "crash area holds no record it understands, state -",
                 (uint32_t)-st, LOG_COLOR_VALUE, "; cleared");
        (void)crashlog_clear();
    }
    klog_u32("CRASHLOG", SEV_OK, "crash area ready, sectors: ",
             (uint32_t)area_sectors, LOG_COLOR_VALUE, "");
}

void crashlog_putc(char c) {
    if (saving) return;                  /* the save's own messages stay out */
    if (text_len < XCRL_TEXT_MAX) text[text_len++] = c;
}

/* Appending to a record's text: the panic's `text`, or a fault record's. */
static void put_to(char *buf, uint32_t *len, const char *s) {
    while (s && *s && *len < XCRL_TEXT_MAX) buf[(*len)++] = *s++;
}
static void put_str(const char *s) { put_to(text, &text_len, s); }

static uint32_t str_len(const char *s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* As many of the latest system-log entries as fit, oldest first. */
static void put_slog_tail(char *buf, uint32_t *len) {
    static const char head[] = "---- system log, latest entries ----\n";
    uint32_t n = slog_count();
    uint32_t room = XCRL_TEXT_MAX - *len;
    if (room < sizeof head) return;
    room -= sizeof head;
    uint32_t start = n, need = 0;
    while (start > 0) {
        const struct slog_entry *e = slog_get(start - 1);
        if (!e) break;
        uint32_t w = str_len(e->system) + 1 + str_len(e->subsystem) + 2 + str_len(e->event) + 1;
        if (need + w > room) break;
        need += w;
        start--;
    }
    put_to(buf, len, head);
    for (uint32_t i = start; i < n; i++) {
        const struct slog_entry *e = slog_get(i);
        if (!e) break;
        put_to(buf, len, e->system);
        if (e->subsystem[0]) { put_to(buf, len, "/"); put_to(buf, len, e->subsystem); }
        put_to(buf, len, ": ");
        put_to(buf, len, e->event);
        put_to(buf, len, "\n");
    }
}

int crashlog_panic_save(void) {
    if (saving) return -1;               /* a panic inside the save */
    if (!have_area) return DISK_ERR_NO_DEVICE;
    saving = 1;
    /* A panic in the middle of a USB transaction leaves the disk unable to
       take the record's commands until it is reset (crashlog.h). The reset
       goes in the record, ahead of the system log so it is never cut off. */
    int reset = disk_panic_reset(area_disk);
    if (reset > 0) put_str(CRASHLOG_RESET_LINE "\n");
    put_slog_tail(text, &text_len);
    int r = crashlog_write_record(XCRL_KIND_CRASH, text, text_len);
    saving = 0;
    return r;
}

int crashlog_test_requested(void) { return test_requested; }

void crashlog_test_interrupt(void) {
    if (!test_requested || !have_area) return;
    if (disk_test_interrupt(area_disk) == DISK_OK)
        klog("CRASHLOG", SEV_WARN, "left a transfer to the crash area's disk half done");
}

int crashlog_recovered(uint32_t *len, uint32_t *crc) {
    if (!recovered) return 0;
    if (len) *len = recovered_len;
    if (crc) *crc = recovered_crc;
    return 1;
}

/* ---- user faults --------------------------------------------------------
 * A user process that faults is ended and the system carries on, so nothing
 * panics and nothing would be saved: on the rig, which has no serial port,
 * the screen was the only record. The fault handler only notes the fault
 * here (no disk I/O from inside an exception), and the process's own thread
 * saves the record once it is back out of ring 3. */
#define FAULT_LINES_MAX 4096u

static char     fault_lines[FAULT_LINES_MAX];
static uint32_t fault_lines_len;
static uint32_t faults_noted, faults_saved;
static volatile int fault_pending;
static int      faults_armed;
static char     fault_rec[XCRL_TEXT_MAX];   /* the record as written */

void crashlog_faults_arm(int on) { faults_armed = on; }

void crashlog_note_fault(const char *line) {
    if (!faults_armed || !have_area || saving) return;
    faults_noted++;
    uint32_t n = str_len(line);
    if (fault_lines_len + n + 1 <= FAULT_LINES_MAX) {
        for (uint32_t i = 0; i < n; i++) fault_lines[fault_lines_len++] = line[i];
        fault_lines[fault_lines_len++] = '\n';
    }                                    /* past the cap only the count grows */
    fault_pending = 1;
}

void crashlog_save_faults(void) {
    if (!fault_pending) return;
    /* Interrupts off for the whole write, as a syscall's disk I/O runs (the
       syscall gate is an interrupt gate), so no other thread's transfer can
       start in the middle of this one. */
    uint32_t f = arch_irq_save();
    if (saving || !fault_pending) { arch_irq_restore(f); return; }
    fault_pending = 0;

    for (uint32_t i = 0; i < XCRL_TEXT_MAX; i++) fault_rec[i] = 0;
    uint32_t len = 0;
    char num[12];
    put_to(fault_rec, &len, "user faults since the self-tests: ");
    num[fmt_u32(num, faults_noted)] = 0;
    put_to(fault_rec, &len, num);
    put_to(fault_rec, &len, "\n");
    for (uint32_t i = 0; i < fault_lines_len && len < XCRL_TEXT_MAX; i++) fault_rec[len++] = fault_lines[i];
    put_slog_tail(fault_rec, &len);
    int r = crashlog_write_record(XCRL_KIND_FAULT, fault_rec, len);
    if (r == DISK_OK) faults_saved++;
    arch_irq_restore(f);

    if (r == DISK_OK) klog_child("saved to the crash area (cxk rig crashlog)");
    else klog_u32("CRASHLOG", SEV_WARN, "user fault not saved, disk error ", (uint32_t)-r, LOG_COLOR_VALUE, "");
}

uint32_t crashlog_faults_saved(void) { return faults_saved; }

void crashlog_faults_forget(void) {
    uint32_t f = arch_irq_save();
    fault_lines_len = 0;
    faults_noted = 0;
    fault_pending = 0;
    arch_irq_restore(f);
}

int crashlog_area(uint8_t *disk, uint64_t *lba, uint64_t *sectors) {
    if (!have_area) return -1;
    if (disk) *disk = area_disk;
    if (lba) *lba = area_lba;
    if (sectors) *sectors = area_sectors;
    return 0;
}

/* ---- the boot log ----------------------------------------------------------
 * The whole system log as text, in the crash partition after the crash record
 * (XCRL_BOOTLOG_LBA), for `cxk rig bootlog`. Rewritten whole each time:
 * text first, header last, as the crash record is, so a save cut short
 * leaves the previous header's CRC failing rather than a log that reads as
 * whole and is not. */
static char     bootlog[XCRL_BOOTLOG_MAX];
static uint8_t  bootlog_hdr[XCRL_SECTOR];
static uint32_t bootlog_saved_total;

static void put_bl(uint32_t *len, const char *s) {
    while (s && *s && *len < XCRL_BOOTLOG_MAX) bootlog[(*len)++] = *s++;
}

static const char *const sev_name[] = { "INFO", "OK  ", "WARN", "ERR ", "FAIL" };

int crashlog_save_bootlog(void) {
    if (!have_area) return DISK_ERR_NO_DEVICE;
    if (area_sectors <= XCRL_BOOTLOG_LBA + 1) return DISK_ERR_NO_DEVICE;
    uint32_t cap = XCRL_BOOTLOG_MAX;
    uint64_t room = (area_sectors - XCRL_BOOTLOG_LBA - 1) * XCRL_SECTOR;
    if (room < cap) cap = (uint32_t)room;

    /* Interrupts off for the whole save, as crashlog_save_faults: no other
       thread's transfer can start in the middle of this one, and the ring
       cannot move under the copy. */
    uint32_t f = arch_irq_save();
    uint32_t len = 0, total = slog_total(), n = slog_count();
    char num[12];
    put_bl(&len, "CXOS system log: ");
    num[fmt_u32(num, n)] = 0;   put_bl(&len, num);
    put_bl(&len, " entries kept of ");
    num[fmt_u32(num, total)] = 0; put_bl(&len, num);
    put_bl(&len, " recorded\n");
    for (uint32_t i = 0; i < n && len < cap; i++) {
        const struct slog_entry *e = slog_get(i);
        if (!e) break;
        if (len + SLOG_SYS_W + SLOG_SUB_W + SLOG_EVT_W + 16 > cap) break;
        put_bl(&len, "[");
        put_bl(&len, e->sev < 5 ? sev_name[e->sev] : "?   ");
        put_bl(&len, "] ");
        put_bl(&len, e->system);
        if (e->subsystem[0]) { put_bl(&len, "/"); put_bl(&len, e->subsystem); }
        put_bl(&len, ": ");
        put_bl(&len, e->event);
        put_bl(&len, "\n");
    }

    int r = DISK_OK;
    uint32_t secs = (len + XCRL_SECTOR - 1) / XCRL_SECTOR;
    for (uint32_t i = len; i < secs * XCRL_SECTOR; i++) bootlog[i] = 0;
    if (secs) r = disk_write(area_disk, area_lba + XCRL_BOOTLOG_LBA + 1, secs, bootlog);
    if (r == DISK_OK) {
        xcrl_seal(bootlog_hdr, XCRL_KIND_BOOTLOG, bootlog, len);
        r = disk_write(area_disk, area_lba + XCRL_BOOTLOG_LBA, 1, bootlog_hdr);
    }
    if (r == DISK_OK) r = disk_flush(area_disk);
    if (r == DISK_OK) bootlog_saved_total = total;
    arch_irq_restore(f);
    return r;
}

int crashlog_bootlog_stale(void) {
    return have_area && slog_total() != bootlog_saved_total;
}
