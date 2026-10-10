// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/disk.c */
/* Aurora Tejeda */
/* Unified disk registry - see disk.h. */

#include "disk.h"
#include "ata.h"
/* Driver availability gates: each storage backend is compiled in only once it's
   been ported/brought up for v5. ATA PIO, AHCI and USB mass storage are all up.
   Flip these to 1 as each lands and add its source to the build. */
#define CXK_HAVE_AHCI 1
#define CXK_HAVE_USB  1

#if CXK_HAVE_AHCI
#include "ahci.h"
#endif
#if CXK_HAVE_USB
/* Mass storage is controller-independent - it rides usb_hc_ops.bulk and works
   the same on EHCI, OHCI or xHCI - so this is the class driver, not a
   controller header as the OHCI-era stub assumed. */
#include "usb_storage.h"
#endif
#include "string.h"

static struct disk disks[DISK_MAX];
static unsigned ndisks = 0;

/* per-media-type index counters, so we get HDD0, HDD1, SSD0, ... */
static int media_index[6];   /* indexed by enum disk_media */

/* base name for a media type */
static const char *media_base(enum disk_media m) {
    switch (m) {
        case DISK_MEDIA_HDD:   return "HDD";
        case DISK_MEDIA_SSD:   return "SSD";
        case DISK_MEDIA_NVME:  return "NVME";
        case DISK_MEDIA_CDROM: return "CDROM";
        case DISK_MEDIA_FDD:   return "FDD";
        default:               return "USB";
    }
}

const char *disk_driver_name(enum disk_driver d) {
    switch (d) {
        case DISK_DRV_ATA:  return "ATA";
        case DISK_DRV_AHCI: return "AHCI";
        case DISK_DRV_NVME: return "NVME";
        case DISK_DRV_USB:  return "USB";
        default:            return "?";
    }
}

/* append a decimal int to a string at *pos (small, 0..99) */
static void append_dec(char *s, int *pos, int v) {
    if (v >= 10) s[(*pos)++] = '0' + (v / 10);
    s[(*pos)++] = '0' + (v % 10);
}

int disk_register(enum disk_driver driver, uint8_t unit,
                  enum disk_media media, enum disk_attach attach,
                  const char *model, uint64_t sectors) {
    if (ndisks >= DISK_MAX) return -1;

    struct disk *d = &disks[ndisks];
    d->id = (uint8_t)ndisks;
    d->driver = driver;
    d->media = media;
    d->attach = attach;
    d->unit = unit;
    d->sectors = sectors;

    /* copy model (bounded) */
    int i = 0;
    if (model) for (; model[i] && i < (int)sizeof(d->model) - 1; i++) d->model[i] = model[i];
    d->model[i] = '\0';

    /* build the name: optional "EXT-" + media base + per-media index.
       The "EXT-" prefix marks external media that ALSO exists internally (so an
       external CD-ROM reads EXT-CDROM0 vs an internal CDROM0). A generic USB
       mass-storage device is named plain USB0 - "USB" already implies external,
       so EXT-USB0 would be redundant. */
    int pos = 0;
    if (attach == DISK_ATTACH_USB && media != DISK_MEDIA_GENERIC) {
        const char *p = "EXT-";
        for (int k = 0; p[k]; k++) d->name[pos++] = p[k];
    }
    const char *base = media_base(media);
    for (int k = 0; base[k]; k++) d->name[pos++] = base[k];
    append_dec(d->name, &pos, media_index[media]);
    d->name[pos] = '\0';
    media_index[media]++;

    ndisks++;
    return d->id;
}

unsigned disk_count(void) { return ndisks; }

const struct disk *disk_get(unsigned i) {
    if (i >= ndisks) return 0;
    return &disks[i];
}

const struct disk *disk_find_by_id(uint8_t id) {
    for (unsigned i = 0; i < ndisks; i++)
        if (disks[i].id == id) return &disks[i];
    return 0;
}

/* case-insensitive compare so disk names are forgiving to type
   (hdd0 / HDD0 / Hdd0 all match). returns 0 if equal. */
static int name_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 1;
        a++; b++;
    }
    return (*a || *b) ? 1 : 0;
}

const struct disk *disk_find_by_name(const char *name) {
    for (unsigned i = 0; i < ndisks; i++)
        if (name_ieq(disks[i].name, name) == 0) return &disks[i];
    return 0;
}

/* ATA here is LBA28, not LBA48: ata.c puts the top nibble of the address in the
   drive-select register (`(lba >> 24) & 0x0F`) and the rest in three 8-bit
   registers, so the addressable range is 0..0x0FFFFFFF - 268435455 sectors, or
   128 GB.

   `(uint32_t)lba` hid that twice over. A 64-bit LBA lost its high half at 4 GB,
   and whatever survived lost four more bits at 128 GB - both silently, and both
   landing the transfer on a real but WRONG sector. For a write that is the worst
   possible failure: it reports success and corrupts somewhere else on the disk.
   An out-of-range address is a caller error, so say so instead.

   The count is checked for the same reason, though no caller can currently trip
   it: disk_read and disk_write split at DISK_XFER_MAX (128), well inside the
   8-bit sector-count register. The check states the driver's own limit rather
   than relying on every future caller knowing it. */
#define ATA_LBA28_MAX 0x0FFFFFFFu

static int ata_range_ok(uint64_t lba, uint32_t count) {
    return lba <= ATA_LBA28_MAX && count > 0 && count <= 255u;
}

static int disk_read_once(const struct disk *d, uint64_t lba, uint32_t count, void *buf) {
    switch (d->driver) {
        case DISK_DRV_ATA:
            if (!ata_range_ok(lba, count)) return DISK_ERR_PARAMS;
            return ata_read(d->unit, (uint32_t)lba, (uint8_t)count, buf);
#if CXK_HAVE_AHCI
        case DISK_DRV_AHCI:
            return ahci_read(d->unit, lba, count, buf);
#endif
#if CXK_HAVE_USB
        case DISK_DRV_USB:
            return usb_storage_read(d->unit, lba, count, buf);
#endif
        default:
            return DISK_ERR_NO_DEVICE;   /* driver not built yet / nvme TODO */
    }
}

static int disk_write_once(const struct disk *d, uint64_t lba, uint32_t count, const void *buf) {
    switch (d->driver) {
        case DISK_DRV_ATA:
            if (!ata_range_ok(lba, count)) return DISK_ERR_PARAMS;
            return ata_write(d->unit, (uint32_t)lba, (uint8_t)count, buf);
#if CXK_HAVE_AHCI
        case DISK_DRV_AHCI:
            return ahci_write(d->unit, lba, count, buf);
#endif
#if CXK_HAVE_USB
        case DISK_DRV_USB:
            return usb_storage_write(d->unit, lba, count, buf);
#endif
        default:
            return DISK_ERR_NO_DEVICE;
    }
}

/* The most sectors one driver call moves. AHCI transfers through a 64 KB
   bounce buffer and refuses more; ATA's sector count is 8 bits, and a larger
   count was silently cut to its low byte. A caller asks for whatever it
   needs - the first-boot install reads each staged file in one call - and is
   served in pieces of this size, so neither limit leaks out of the drivers. */
#define DISK_XFER_MAX 128u

/* The checks both entry points owe the contract, in one place so they cannot
   drift apart. Writing the contract down (disk.h) is what exposed these: the
   enum already documented PARAMS as covering "null buffer, zero count" and
   BOUNDS as "LBA/count outside the device", and neither was checked here.
   A null buffer went to the driver, which would have written 512 bytes to
   address 0; a zero count returned DISK_OK having done nothing, which the
   enum says is an error; an unknown id returned a bare -1 (GENERIC), so a
   caller could not tell "no such disk" from "I/O error"; and nothing compared
   the request against the capacity the registry already knew, so only ATA
   caught an over-range LBA and only because of its own narrower limit. */
static int disk_check(const struct disk *d, uint64_t lba, uint32_t count,
                      const void *buf) {
    if (!d)              return DISK_ERR_NO_DEVICE;
    if (!buf)            return DISK_ERR_PARAMS;
    if (count == 0)      return DISK_ERR_PARAMS;
    /* Overflow-safe, and both halves are load-bearing. The subtraction form
       avoids summing lba + count, which a count near 2^32 could wrap past the
       ceiling - but it is only valid once lba is known to be inside the
       device, because otherwise `d->sectors - lba` underflows to something
       enormous and any count compares smaller than it. So the first test
       catches an LBA past the end and the second a count that runs off it;
       remove either and one of the two cases goes unguarded. The two are
       covered by different tests for that reason - the contract test reads
       exactly one sector past the end, the ATA range test reads far past it. */
    if (lba >= d->sectors)                  return DISK_ERR_BOUNDS;
    if ((uint64_t)count > d->sectors - lba) return DISK_ERR_BOUNDS;
    return DISK_OK;
}

int disk_read(uint8_t id, uint64_t lba, uint32_t count, void *buf) {
    const struct disk *d = disk_find_by_id(id);
    int bad = disk_check(d, lba, count, buf);
    if (bad != DISK_OK) return bad;
    uint8_t *p = (uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count < DISK_XFER_MAX ? count : DISK_XFER_MAX;
        int rc = disk_read_once(d, lba, n, p);
        if (rc != 0) return rc;
        lba += n; p += n * 512u; count -= n;
    }
    return 0;
}

int disk_write(uint8_t id, uint64_t lba, uint32_t count, const void *buf) {
    const struct disk *d = disk_find_by_id(id);
    int bad = disk_check(d, lba, count, buf);
    if (bad != DISK_OK) return bad;
    const uint8_t *p = (const uint8_t *)buf;
    while (count > 0) {
        uint32_t n = count < DISK_XFER_MAX ? count : DISK_XFER_MAX;
        int rc = disk_write_once(d, lba, n, p);
        if (rc != 0) return rc;
        lba += n; p += n * 512u; count -= n;
    }
    return 0;
}

/* human-readable text for a disk_err code */
const char *disk_err_str(int err) {
    switch (err) {
        case DISK_OK:            return "ok";
        case DISK_ERR_NO_DEVICE: return "no such device";
        case DISK_ERR_TIMEOUT:   return "device timeout";
        case DISK_ERR_NOT_READY: return "device not ready";
        case DISK_ERR_FAULT:     return "device fault";
        case DISK_ERR_BOUNDS:    return "out of bounds";
        case DISK_ERR_PARAMS:    return "bad parameters";
        case DISK_ERR_GENERIC:   return "I/O error";
        default:                 return "unknown error";
    }
}

/* sectors*512 -> "N KB/MB/GB". Uses only shifts (no 64-bit divide, which would
   need libgcc's __udivdi3/__umoddi3 that a freestanding kernel doesn't link).
   Capacity boundaries are powers of two in sectors:
     1 KB = 2 sectors      -> >>1
     1 MB = 2048 sectors   -> >>11
     1 GB = 2097152 sectors-> >>21
   We pick the largest unit that yields a non-zero value. */
void disk_capacity_str(uint64_t sectors, char *buf, int cap) {
    uint64_t val;
    const char *unit;

    /* The bounded-string contract (string.h). cap == 0 wrote a terminator at
       buf[0] anyway, one byte past a zero-length buffer, and a negative cap
       did the same; both are now nothing at all. */
    if (!buf || cap <= 0) return;
    buf[0] = '\0';

    if (sectors >= (2097152ull)) {        /* >= 1 GB */
        val = sectors >> 21; unit = "GB";
    } else if (sectors >= 2048ull) {      /* >= 1 MB */
        val = sectors >> 11; unit = "MB";
    } else if (sectors >= 2ull) {         /* >= 1 KB */
        val = sectors >> 1;  unit = "KB";
    } else {
        val = sectors * 512ull; unit = "B";
    }

    /* val now fits comfortably in 32 bits; format with 32-bit math (no 64-bit
       division). Build digits reversed in tmp, using a 32-bit working copy. */
    uint32_t v = (uint32_t)val;
    char tmp[16];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }

    /* Rule 3: a void function must not truncate. Work out whether the whole
       thing fits first - digits, a space, the unit, the terminator - and
       leave the buffer empty if it does not, rather than emitting a fragment
       that reads as complete. A small buffer used to get the unit with no
       number in front of it, so a 500 GB disk displayed as "GB". */
    int unit_len = 0;
    while (unit[unit_len]) unit_len++;
    if (n + 1 + unit_len + 1 > cap) return;   /* already "" from above */

    int p = 0;
    while (n > 0) buf[p++] = tmp[--n];
    buf[p++] = ' ';
    for (int k = 0; unit[k]; k++) buf[p++] = unit[k];
    buf[p] = '\0';
}