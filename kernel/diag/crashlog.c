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

#define CRASH_FILE "/System/crash.log"

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

/* A previous boot's record into /System/crash.log. The area is cleared only
   once the file is written, so a failure here loses nothing. */
static void recover(uint32_t len) {
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
    int id = cxfs_resolve(CRASH_FILE, 0);
    if (id < 0) {
        int dir = cxfs_resolve("/System", 0);
        if (dir >= 0) id = cxfs_create_entry((uint32_t)dir, "crash.log", CXFS_TYPE_FILE);
    }
    if (id < 0 || cxfs_write_file((uint32_t)id, text, len) != 0) {
        klog("CRASHLOG", SEV_WARN, "previous crash log kept: " CRASH_FILE " not written");
        clear_text();
        return;
    }
    klog("CRASHLOG", SEV_WARN, "recovered the previous boot's crash log to " CRASH_FILE);
    log_reason();
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
    } else if (st == XCRL_VALID) {
        recover(len);
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

static void put_str(const char *s) {
    while (s && *s && text_len < XCRL_TEXT_MAX) text[text_len++] = *s++;
}

static uint32_t str_len(const char *s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* As many of the latest system-log entries as fit, oldest first. */
static void put_slog_tail(void) {
    static const char head[] = "---- system log, latest entries ----\n";
    uint32_t n = slog_count();
    uint32_t room = XCRL_TEXT_MAX - text_len;
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
    put_str(head);
    for (uint32_t i = start; i < n; i++) {
        const struct slog_entry *e = slog_get(i);
        if (!e) break;
        put_str(e->system);
        if (e->subsystem[0]) { put_str("/"); put_str(e->subsystem); }
        put_str(": ");
        put_str(e->event);
        put_str("\n");
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
    put_slog_tail();
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

int crashlog_area(uint8_t *disk, uint64_t *lba, uint64_t *sectors) {
    if (!have_area) return -1;
    if (disk) *disk = area_disk;
    if (lba) *lba = area_lba;
    if (sectors) *sectors = area_sectors;
    return 0;
}
