// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/ata.c */
/* Aurora Tejeda */
/* ATA PIO driver - primary + secondary channels, 28-bit LBA, 512-byte sectors.
 *
 * Drive numbering:
 *   0 = primary master    (ports 0x1F0, drive byte 0xE0)
 *   1 = primary slave     (ports 0x1F0, drive byte 0xF0)
 *   2 = secondary master  (ports 0x170, drive byte 0xE0)
 *   3 = secondary slave   (ports 0x170, drive byte 0xF0)
 *
 * SATA controllers in IDE/legacy mode map their ports across both channels,
 * so a drive on "SATA4" may land on the secondary channel - hence we probe
 * both. (AHCI mode would hide drives from these legacy ports entirely.)
 */

#include "ata.h"
#include "timer.h"
#include "disk.h"
#include "logging.h"
#include "io.h"

/* per-channel I/O base ports */
#define PRI_BASE   0x1F0
#define PRI_CTRL   0x3F6
#define SEC_BASE   0x170
#define SEC_CTRL   0x376

/* register offsets from a channel's base port */
#define REG_DATA       0       /* read/write data (16-bit) */
#define REG_ERROR      1
#define REG_SECCOUNT   2
#define REG_LBA_LO     3
#define REG_LBA_MID    4
#define REG_LBA_HI     5
#define REG_DRIVE      6       /* drive/head select */
#define REG_STATUS     7       /* read: status / write: command */
#define REG_COMMAND    7

/* status register bits */
#define ATA_SR_BSY      0x80
#define ATA_SR_DRDY     0x40
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

/* commands */
#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_FLUSH    0xE7
#define ATA_CMD_IDENTIFY 0xEC

#define ATA_DRIVE_COUNT 4

static int      drive_present[ATA_DRIVE_COUNT] = { 0, 0, 0, 0 };
static char     drive_model[ATA_DRIVE_COUNT][41];
static uint32_t drive_sectors[ATA_DRIVE_COUNT] = { 0, 0, 0, 0 };
static uint8_t  drive_ssd[ATA_DRIVE_COUNT] = { 0, 0, 0, 0 };  /* 1 = solid-state */

/* --- 16-bit port helpers: inw/outw now come from io.h --- */

/* channel base port for a drive (0,1 -> primary; 2,3 -> secondary) */
static uint16_t drive_base(uint8_t drive) {
    return (drive < 2) ? PRI_BASE : SEC_BASE;
}
/* is this drive the slave on its channel? (odd numbers) */
static int drive_is_slave(uint8_t drive) {
    return drive & 1;
}

/* fallback spin bound retained as a safety net for the timer-off spin path */
#define ATA_TIMEOUT 1000000
/* wall-clock budget for ATA waits (ms). Generous for slow real drives. */
#define ATA_TIMEOUT_MS 3000

/* wait while BSY is set. returns DISK_OK when cleared, DISK_ERR_NO_DEVICE on a
   floating bus (0xFF), DISK_ERR_TIMEOUT if it never clears in time. */
static int ata_wait_busy(uint16_t base) {
    struct timeout to;
    timer_timeout_start(&to, ATA_TIMEOUT_MS);
    for (;;) {
        uint8_t s = inb(base + REG_STATUS);
        if (s == 0xFF) return DISK_ERR_NO_DEVICE;   /* floating bus: no drive */
        if (!(s & ATA_SR_BSY)) return DISK_OK;
        if (timer_timeout_expired(&to)) return DISK_ERR_TIMEOUT;
    }
}

/* wait for DRQ. returns DISK_OK when ready, DISK_ERR_NO_DEVICE on floating bus,
   DISK_ERR_FAULT if the device sets ERR, DISK_ERR_TIMEOUT on timeout. */
static int ata_wait_drq(uint16_t base) {
    struct timeout to;
    timer_timeout_start(&to, ATA_TIMEOUT_MS);
    for (;;) {
        uint8_t s = inb(base + REG_STATUS);
        if (s == 0xFF) return DISK_ERR_NO_DEVICE;
        if (s & ATA_SR_ERR) return DISK_ERR_FAULT;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return DISK_OK;
        if (timer_timeout_expired(&to)) return DISK_ERR_TIMEOUT;
    }
}

/* select drive on its channel and set high LBA bits.
   0xE0 = LBA + master, 0xF0 = LBA + slave. */
static void ata_select(uint8_t drive, uint32_t lba) {
    uint16_t base = drive_base(drive);
    uint8_t d = (drive_is_slave(drive) ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F);
    outb(base + REG_DRIVE, d);
}

/* probe a single drive position; fills the info tables. */
static void ata_probe(uint8_t drive) {
    uint16_t base = drive_base(drive);

    ata_select(drive, 0);
    for (int i = 0; i < 4; i++) inb(base + REG_STATUS);   /* small delay */

    outb(base + REG_SECCOUNT, 0);
    outb(base + REG_LBA_LO, 0);
    outb(base + REG_LBA_MID, 0);
    outb(base + REG_LBA_HI, 0);
    outb(base + REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(base + REG_STATUS);
    if (status == 0 || status == 0xFF) { drive_present[drive] = 0; return; }

    if (ata_wait_busy(base) != 0) { drive_present[drive] = 0; return; }

    /* ATA disks keep LBA_MID/HI at 0; nonzero = ATAPI/other signature */
    if (inb(base + REG_LBA_MID) != 0 || inb(base + REG_LBA_HI) != 0) {
        drive_present[drive] = 0;
        return;
    }

    if (ata_wait_drq(base) == 0) {
        uint16_t id[256];
        for (int i = 0; i < 256; i++) id[i] = inw(base + REG_DATA);

        for (int i = 0; i < 20; i++) {
            uint16_t w = id[27 + i];
            drive_model[drive][i * 2]     = (char)(w >> 8);
            drive_model[drive][i * 2 + 1] = (char)(w & 0xFF);
        }
        drive_model[drive][40] = '\0';
        for (int i = 39; i >= 0 && drive_model[drive][i] == ' '; i--)
            drive_model[drive][i] = '\0';

        drive_sectors[drive] = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
        /* word 217 = nominal media rotation rate; 0x0001 = non-rotating (SSD) */
        drive_ssd[drive] = (id[217] == 0x0001) ? 1 : 0;
        drive_present[drive] = 1;
    } else {
        drive_present[drive] = 0;
    }
}

void ata_init(void) {
    for (uint8_t drive = 0; drive < ATA_DRIVE_COUNT; drive++)
        ata_probe(drive);

    /* Bridge probe results into the unified disk registry. Without this, drives
       are detected (drive_present[]) but never become disk_count()-visible
       entries, so the filesystem layer can't find them. Register each present
       drive as an internal ATA disk; media = SSD or HDD per the IDENTIFY hint. */
    for (uint8_t drive = 0; drive < ATA_DRIVE_COUNT; drive++) {
        if (!drive_present[drive]) continue;
        enum disk_media media = drive_ssd[drive] ? DISK_MEDIA_SSD : DISK_MEDIA_HDD;
        /* A full registry (DISK_MAX) refuses, and a drive that was probed but
           not registered is invisible to every layer above - so say so, rather
           than leaving the boot log's disk count to be noticed as one short. */
        if (disk_register(DISK_DRV_ATA, drive, media, DISK_ATTACH_INTERNAL,
                          drive_model[drive], (uint64_t)drive_sectors[drive]) < 0)
            klog_u32("ATA", SEV_WARN, "disk registry full, drive not registered: ",
                     (uint32_t)drive, LOG_COLOR_VALUE, "");
    }
}

int ata_present(uint8_t drive) {
    if (drive >= ATA_DRIVE_COUNT) return 0;
    return drive_present[drive];
}

const char *ata_model(uint8_t drive) {
    if (drive >= ATA_DRIVE_COUNT || !drive_present[drive]) return "";
    return drive_model[drive];
}

int ata_is_ssd(uint8_t drive) {
    if (drive >= ATA_DRIVE_COUNT || !drive_present[drive]) return 0;
    return drive_ssd[drive];
}

uint32_t ata_sectors(uint8_t drive) {
    if (drive >= ATA_DRIVE_COUNT) return 0;
    return drive_sectors[drive];
}

int ata_read(uint8_t drive, uint32_t lba, uint8_t count, void *buffer) {
    if (drive >= ATA_DRIVE_COUNT || count == 0 || !buffer) return DISK_ERR_PARAMS;
    if (!drive_present[drive]) return DISK_ERR_NO_DEVICE;
    uint16_t base = drive_base(drive);
    uint16_t *buf = (uint16_t *)buffer;

    int r = ata_wait_busy(base);
    if (r != DISK_OK) return r;
    ata_select(drive, lba);
    outb(base + REG_SECCOUNT, count);
    outb(base + REG_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(base + REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + REG_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    outb(base + REG_COMMAND, ATA_CMD_READ);

    for (int s = 0; s < count; s++) {
        r = ata_wait_drq(base);
        if (r != DISK_OK) return r;
        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++)
            *buf++ = inw(base + REG_DATA);
    }
    return DISK_OK;
}

int ata_write(uint8_t drive, uint32_t lba, uint8_t count, const void *buffer) {
    if (drive >= ATA_DRIVE_COUNT || count == 0 || !buffer) return DISK_ERR_PARAMS;
    if (!drive_present[drive]) return DISK_ERR_NO_DEVICE;
    uint16_t base = drive_base(drive);
    const uint16_t *buf = (const uint16_t *)buffer;

    int r = ata_wait_busy(base);
    if (r != DISK_OK) return r;
    ata_select(drive, lba);
    outb(base + REG_SECCOUNT, count);
    outb(base + REG_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(base + REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + REG_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    outb(base + REG_COMMAND, ATA_CMD_WRITE);

    for (int s = 0; s < count; s++) {
        r = ata_wait_drq(base);
        if (r != DISK_OK) return r;
        for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++)
            outw(base + REG_DATA, *buf++);
    }

    outb(base + REG_COMMAND, ATA_CMD_FLUSH);
    r = ata_wait_busy(base);
    if (r != DISK_OK) return r;
    return DISK_OK;
}
/* Tell the drive to commit its write cache to the platter.
 *
 * ata_write already issues FLUSH CACHE after every transfer, so on this
 * backend a caller that got DISK_OK from a write is already durable. This
 * exists anyway, because durability has to be something a caller can ASK for
 * rather than something one driver happens to do - AHCI and USB did not, and a
 * guarantee that depends on which cable the disk is on is not a guarantee.
 * Both NT and XNU plumb flush as its own operation through every layer for the
 * same reason (IRP_MJ_FLUSH_BUFFERS, DKIOCSYNCHRONIZECACHE). */
int ata_flush(uint8_t drive) {
    if (drive >= ATA_DRIVE_COUNT) return DISK_ERR_PARAMS;
    if (!drive_present[drive]) return DISK_ERR_NO_DEVICE;
    uint16_t base = drive_base(drive);

    int r = ata_wait_busy(base);
    if (r != DISK_OK) return r;
    ata_select(drive, 0);
    outb(base + REG_COMMAND, ATA_CMD_FLUSH);
    return ata_wait_busy(base);
}
