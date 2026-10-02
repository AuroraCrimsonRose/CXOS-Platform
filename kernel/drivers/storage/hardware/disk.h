// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/disk.h */
/* Aurora Tejeda */
/*
 * Unified disk registry.
 *
 * Sits above the individual storage drivers (ATA PIO, AHCI, and later NVMe,
 * USB mass storage, ...). Each driver registers the disks it finds; the
 * registry assigns a stable name (HDD0, SSD0, EXT-HDD0, ...) and a hex ID,
 * and provides ONE read/write interface that dispatches to the owning driver.
 *
 * The rest of the kernel (CXFS, the shell) talks to disks by ID or name and
 * never needs to know which driver is underneath. New storage types just call
 * disk_register() and appear automatically.
 */

#ifndef DISK_H
#define DISK_H

#include <stdint.h>

#define DISK_MAX        16
#define DISK_NAME_LEN   16

/* which driver communicates with this disk */
enum disk_driver {
    DISK_DRV_ATA = 0,    /* legacy PIO ATA  */
    DISK_DRV_AHCI,       /* AHCI SATA       */
    DISK_DRV_NVME,       /* (future)        */
    DISK_DRV_USB         /* USB mass storage (future) */
};

/* the kind of medium (drives the name prefix) */
enum disk_media {
    DISK_MEDIA_HDD = 0,  /* rotating disk      -> HDD  */
    DISK_MEDIA_SSD,      /* solid-state        -> SSD  */
    DISK_MEDIA_NVME,     /* nvme               -> NVME */
    DISK_MEDIA_CDROM,    /* optical            -> CDROM*/
    DISK_MEDIA_FDD,      /* floppy             -> FDD  */
    DISK_MEDIA_GENERIC   /* unknown mass store -> USB  */
};

/* how it's attached (USB-attached devices get an EXT- prefix) */
enum disk_attach {
    DISK_ATTACH_INTERNAL = 0,
    DISK_ATTACH_USB              /* external / USB-attached -> EXT- prefix */
};

/* disk operation result codes. 0 = success; negative = error. The drivers
   return these so failures are diagnosable (not just a generic -1). */
enum disk_err {
    DISK_OK            =  0,    /* success */
    DISK_ERR_GENERIC   = -1,    /* unspecified failure */
    DISK_ERR_NO_DEVICE = -2,    /* no such disk / not present */
    DISK_ERR_TIMEOUT   = -3,    /* hardware did not respond in time */
    DISK_ERR_NOT_READY = -4,    /* device busy / not ready */
    DISK_ERR_FAULT     = -5,    /* device reported an error (ERR/DF/TFES) */
    DISK_ERR_BOUNDS    = -6,    /* LBA/count outside the device */
    DISK_ERR_PARAMS    = -7     /* bad arguments (null buffer, zero count) */
};

struct disk {
    uint8_t  id;                      /* numeric id (shown as hex) */
    char     name[DISK_NAME_LEN];     /* assigned: HDD0, EXT-CDROM0, ... */
    enum disk_driver driver;
    enum disk_media  media;
    enum disk_attach attach;
    uint8_t  unit;                    /* driver-local index (ata drive / ahci port) */
    uint64_t sectors;                 /* capacity in 512-byte sectors */
    char     model[44];
};

/* called by drivers at boot to register a discovered disk. returns the new
   disk's id, or -1 if the table is full. */
int disk_register(enum disk_driver driver, uint8_t unit,
                  enum disk_media media, enum disk_attach attach,
                  const char *model, uint64_t sectors);

unsigned        disk_count(void);
const struct disk *disk_get(unsigned i);            /* by table index */
const struct disk *disk_find_by_id(uint8_t id);     /* by hex id */
const struct disk *disk_find_by_name(const char *name);

/* unified block I/O - dispatches to the owning driver. returns a disk_err
   code (DISK_OK on success, negative DISK_ERR_* on failure). */
int disk_read (uint8_t id, uint64_t lba, uint32_t count, void *buf);
int disk_write(uint8_t id, uint64_t lba, uint32_t count, const void *buf);

/* human-readable text for a disk_err code (e.g. "timeout", "not ready"). */
const char *disk_err_str(int err);

/* human-readable capacity (e.g. "16 MB", "476 GB") into buf */
void disk_capacity_str(uint64_t sectors, char *buf, int cap);

/* driver name string for display ("ATA", "AHCI", ...) */
const char *disk_driver_name(enum disk_driver d);

#endif