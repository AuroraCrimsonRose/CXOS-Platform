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

/* ====================================================================
 * THE BLOCK-DEVICE CONTRACT (engineering §8)
 *
 * Frozen here before the VFS and block cache start depending on whatever the
 * current backends happen to do. Every driver registering through
 * disk_register() promises all of this, and ATA, AHCI and USB mass storage
 * are held to it by test_disk_contract in ktest.c.
 *
 *   Sector size        512 bytes, always, for every medium - including
 *                      CD-ROM, whose native 2048-byte blocks the driver
 *                      presents as four 512-byte sectors. `sectors` in struct
 *                      disk is a count of these, so capacity is
 *                      sectors * 512 and nothing above needs to ask.
 *
 *   LBA width          64 bits at this interface. Each backend narrows it and
 *                      REFUSES rather than truncates what it cannot address:
 *                      ATA here is LBA28 (the top nibble goes in the
 *                      drive-select register), so its ceiling is 0x0FFFFFFF,
 *                      128 GB; AHCI is LBA48. A request past a backend's
 *                      limit is DISK_ERR_BOUNDS, never a transfer aimed
 *                      somewhere else - which is what the pre-Phase-1 cast to
 *                      32 bits did, landing writes on real but wrong sectors.
 *                      The generic check is here, against the capacity the
 *                      registry already holds, so every backend refuses an
 *                      unreachable sector and not just the one whose own
 *                      limit happens to be narrowest.
 *
 *   Max transfer       Unlimited at this interface: disk_read/disk_write
 *                      split requests into DISK_XFER_MAX (128) sector pieces
 *                      and loop. Callers do not chunk. The figure is not
 *                      arbitrary - it is AHCI's 64 KB bounce buffer, and it
 *                      is also inside ATA's 8-bit sector count, so one
 *                      constant satisfies both.
 *
 *   Alignment          None. A buffer at any address works. This is the one
 *                      promise that costs something: AHCI DMAs through its
 *                      own physically contiguous bounce buffer and memcpys to
 *                      the caller, precisely so a caller never has to know
 *                      whether its buffer is contiguous in physical memory.
 *                      NT exposes the opposite choice and makes the caller
 *                      conform - STORAGE_ADAPTER_DESCRIPTOR carries
 *                      AlignmentMask, MaximumTransferLength and
 *                      MaximumPhysicalPages for the caller to query
 *                      (ntddstor.h:645). That is the better design once there
 *                      are enough backends for the weakest one to be a real
 *                      cost; it is the wrong trade at three.
 *
 *   Buffers            Kernel virtual addresses, no DMA properties required
 *                      (see Alignment). A user pointer is never passed here:
 *                      the syscall layer copies through the kernel first.
 *
 *   Synchronous        Every call completes, or fails, before it returns.
 *                      There is no queue, no completion callback and no
 *                      in-flight state, so a caller may reuse its buffer
 *                      immediately. Introducing async means a new entry
 *                      point, not a changed meaning for these.
 *
 *   Ordering           A write that returned DISK_OK is readable by the next
 *                      read. There is no write cache in this layer; whether
 *                      the device has one is not modelled, so this is not a
 *                      durability guarantee across power loss and nothing
 *                      should read it as one. A flush/FUA op is the way that
 *                      gets added.
 *
 *   Error codes        The disk_err enum below, never a bare -1. Distinct
 *                      causes stay distinct: BOUNDS (outside the device) is
 *                      not PARAMS (a nonsense request) is not NO_DEVICE (no
 *                      such disk) is not TIMEOUT (hardware silent) is not
 *                      FAULT (hardware refused). Callers may branch on them.
 *
 *   Lifetime           A registration lasts for the boot. Ids and names are
 *                      stable from disk_register() until shutdown, and
 *                      `const struct disk *` handed out by disk_get() and
 *                      friends stays valid for the same span.
 *
 *                      There is deliberately NO disk_unregister(): removal is
 *                      not implemented, so pulling a USB disk leaves a
 *                      registered entry whose transfers fail with TIMEOUT or
 *                      NO_DEVICE. Stated rather than discovered, because a
 *                      caching layer written against this must not assume
 *                      a device it holds an id for is still physically there.
 *                      Adding hotplug means adding removal notification
 *                      here, and every holder of an id becomes a holder of a
 *                      reference - the same problem IPC endpoints already
 *                      solved by counting (security §7).
 * ==================================================================== */

#ifndef DISK_H
#define DISK_H

#include <stdint.h>

/* Sector size, in bytes. Fixed for every medium - see the contract above. */
#define DISK_SECTOR_SIZE 512u

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