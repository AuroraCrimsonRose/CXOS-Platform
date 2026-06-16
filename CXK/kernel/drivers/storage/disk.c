/* /CXLite/kernel/drivers/disk.c */
/* Aurora Tejeda */
/* Unified disk registry - see disk.h. */

#include "disk.h"
#include "ata.h"
#include "ahci.h"
#include "ohci.h"
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

int disk_read(uint8_t id, uint64_t lba, uint32_t count, void *buf) {
    const struct disk *d = disk_find_by_id(id);
    if (!d) return -1;
    switch (d->driver) {
        case DISK_DRV_ATA:
            return ata_read(d->unit, (uint32_t)lba, (uint8_t)count, buf);
        case DISK_DRV_AHCI:
            return ahci_read(d->unit, lba, count, buf);
        case DISK_DRV_USB:
            return ohci_storage_read(lba, count, buf);
        default:
            return -1;   /* nvme not implemented yet */
    }
}

int disk_write(uint8_t id, uint64_t lba, uint32_t count, const void *buf) {
    const struct disk *d = disk_find_by_id(id);
    if (!d) return -1;
    switch (d->driver) {
        case DISK_DRV_ATA:
            return ata_write(d->unit, (uint32_t)lba, (uint8_t)count, buf);
        case DISK_DRV_AHCI:
            return ahci_write(d->unit, lba, count, buf);
        case DISK_DRV_USB:
            return ohci_storage_write(lba, count, buf);
        default:
            return -1;
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

    int p = 0;
    while (n > 0 && p < cap - 4) buf[p++] = tmp[--n];
    if (p < cap - 4) buf[p++] = ' ';
    for (int k = 0; unit[k] && p < cap - 1; k++) buf[p++] = unit[k];
    buf[p] = '\0';
}