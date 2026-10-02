// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/usb/usb_storage.c */
/* Aurora Tejeda / CATX Systems */
/* USB mass storage: Bulk-Only Transport carrying SCSI. See usb_storage.h. */

#include "usb_storage.h"
#include "usb.h"
#include "disk.h"
#include "logging.h"
#include "timer.h"

#define CBW_SIGNATURE   0x43425355u   /* "USBC" */
#define CSW_SIGNATURE   0x53425355u   /* "USBS" */

#define CBW_FLAG_IN     0x80

struct bot_cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_transfer_length;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cb_length;
    uint8_t  cb[16];
} __attribute__((packed));

struct bot_csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t  status;          /* 0 = passed, 1 = failed, 2 = phase error */
} __attribute__((packed));

_Static_assert(sizeof(struct bot_cbw) == 31, "a CBW is 31 bytes on the wire");
_Static_assert(sizeof(struct bot_csw) == 13, "a CSW is 13 bytes on the wire");

/* SCSI opcodes */
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY10  0x25
#define SCSI_READ10           0x28
#define SCSI_WRITE10          0x2A

#define MAX_UNITS 4
#define USB_BLOCK_MAX 4096        /* one bulk transfer's worth */

struct storage_unit {
    struct usb_device *dev;
    uint8_t  in_use;
    uint8_t  lun;
    uint32_t block_size;
    uint8_t  sector_shift;    /* log2(block_size / 512) - see the note in rw() */
    uint64_t blocks;
    uint8_t  disk_id;
};

static struct storage_unit units[MAX_UNITS];
static uint32_t next_tag = 1;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* CLEAR_FEATURE(ENDPOINT_HALT). A stalled bulk endpoint stays stalled until it
   is cleared, and the clear resets the toggle on the DEVICE - so the host's
   copy has to be reset to match or every packet after is silently dropped. */
static void clear_stall(struct usb_device *dev, uint8_t ep) {
    dev->ops->control(dev, 0x02, 0x01 /* CLEAR_FEATURE */, 0 /* ENDPOINT_HALT */, ep, 0, 0);
    if (dev->ops->reset_toggle) dev->ops->reset_toggle(dev, ep);
}

/* One complete Bulk-Only transaction: CBW, optional data, CSW.
   Returns the bytes transferred in the data phase, or -1. */
static int bot_transfer(struct storage_unit *u, const uint8_t *cdb, uint8_t cdb_len,
                        void *data, uint32_t len, int data_in) {
    struct usb_device *dev = u->dev;
    struct bot_cbw cbw;
    for (unsigned i = 0; i < sizeof(cbw); i++) ((uint8_t *)&cbw)[i] = 0;

    uint32_t tag = next_tag++;
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = tag;
    cbw.data_transfer_length = len;
    cbw.flags = data_in ? CBW_FLAG_IN : 0;
    cbw.lun = u->lun;
    cbw.cb_length = cdb_len;
    for (unsigned i = 0; i < cdb_len && i < 16; i++) cbw.cb[i] = cdb[i];

    if (dev->ops->bulk(dev, dev->ep_out, &cbw, sizeof(cbw)) != (int)sizeof(cbw)) {
        clear_stall(dev, dev->ep_out);
        return -1;
    }

    int moved = 0;
    if (len && data) {
        uint8_t ep = data_in ? dev->ep_in : dev->ep_out;
        moved = dev->ops->bulk(dev, ep, data, len);
        if (moved < 0) {
            /* A stall in the data phase is recoverable: clear it and still
               collect the CSW, which is what tells us what actually happened. */
            clear_stall(dev, ep);
            moved = 0;
        }
    }

    struct bot_csw csw;
    for (unsigned i = 0; i < sizeof(csw); i++) ((uint8_t *)&csw)[i] = 0;
    int got = dev->ops->bulk(dev, dev->ep_in, &csw, sizeof(csw));
    if (got != (int)sizeof(csw)) {
        clear_stall(dev, dev->ep_in);
        got = dev->ops->bulk(dev, dev->ep_in, &csw, sizeof(csw));   /* one retry */
        if (got != (int)sizeof(csw)) return -1;
    }

    if (csw.signature != CSW_SIGNATURE) return -1;
    /* A mismatched tag means host and device have lost step with each other -
       the reply belongs to some earlier command, so nothing here is trustworthy. */
    if (csw.tag != tag) return -1;
    if (csw.status != 0) return -1;

    return moved;
}

static int scsi_test_unit_ready(struct storage_unit *u) {
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return bot_transfer(u, cdb, sizeof(cdb), 0, 0, 1) < 0 ? -1 : 0;
}

static int scsi_request_sense(struct storage_unit *u, uint8_t *sense18) {
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    return bot_transfer(u, cdb, sizeof(cdb), sense18, 18, 1);
}

static int scsi_inquiry(struct storage_unit *u, uint8_t *out36) {
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    return bot_transfer(u, cdb, sizeof(cdb), out36, 36, 1);
}

static int scsi_read_capacity(struct storage_unit *u) {
    uint8_t cdb[10] = { SCSI_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t cap[8];
    for (int i = 0; i < 8; i++) cap[i] = 0;
    if (bot_transfer(u, cdb, sizeof(cdb), cap, 8, 1) < 8) return -1;

    /* READ CAPACITY(10) returns the LAST addressable block, not the count -
       an off-by-one here loses or invents a sector at the end of the device. */
    uint32_t last_lba = be32(&cap[0]);
    u->block_size = be32(&cap[4]);
    if (u->block_size < 512) return -1;

    /* The disk layer addresses 512-byte sectors, so converting between its LBAs
       and the device's means dividing a 64-bit LBA - and a 32-bit freestanding
       kernel has no __udivdi3 to do that with. Store the ratio as a SHIFT
       instead, which needs no libgcc and is exact.
       That requires a power-of-two block size. Every real device has one (512,
       1024, 2048, 4096); anything else is refused rather than silently
       mis-addressed. */
    uint32_t ratio = u->block_size / 512;
    if (ratio * 512 != u->block_size || (ratio & (ratio - 1)) != 0) return -1;
    uint8_t shift = 0;
    while ((1u << shift) < ratio) shift++;
    u->sector_shift = shift;

    u->blocks = (uint64_t)last_lba + 1;
    return 0;
}

static int scsi_rw10(struct storage_unit *u, uint64_t lba, uint16_t blocks,
                     void *buf, int read) {
    uint8_t cdb[10];
    for (int i = 0; i < 10; i++) cdb[i] = 0;
    cdb[0] = read ? SCSI_READ10 : SCSI_WRITE10;
    put_be32(&cdb[2], (uint32_t)lba);
    put_be16(&cdb[7], blocks);
    uint32_t len = (uint32_t)blocks * u->block_size;
    return bot_transfer(u, cdb, sizeof(cdb), buf, len, read);
}

/* ---- attach ------------------------------------------------------------- */
int usb_storage_attach(struct usb_device *dev) {
    if (!dev || !dev->ops || !dev->ops->bulk) return 0;
    if (dev->if_class != USB_CLASS_MASS_STORAGE) return 0;
    if (dev->if_protocol != USB_PROTO_BULK_ONLY) {
        klog("USBSTOR", SEV_INFO, "mass storage present but not bulk-only - skipped");
        return 0;
    }
    if (!dev->ep_in || !dev->ep_out) {
        klog("USBSTOR", SEV_WARN, "mass storage without both bulk endpoints - skipped");
        return 0;
    }

    struct storage_unit *u = 0;
    uint8_t unit_index = 0;
    for (uint8_t i = 0; i < MAX_UNITS; i++)
        if (!units[i].in_use) { u = &units[i]; unit_index = i; break; }
    if (!u) return 0;

    for (unsigned i = 0; i < sizeof(*u); i++) ((uint8_t *)u)[i] = 0;
    u->dev = dev;
    u->lun = 0;
    u->in_use = 1;

    /* GET_MAX_LUN is a class request, and a device is allowed to STALL it
       rather than answer - which means "one LUN", not an error. */
    uint8_t maxlun = 0;
    dev->ops->control(dev, 0xA1, 0xFE, 0, 0, &maxlun, 1);

    /* A flash drive is often not ready on the first ask. The spec's answer is
       REQUEST SENSE, then try again; this settles a "medium not present" that
       is really just spin-up. */
    for (int i = 0; i < 10; i++) {
        if (scsi_test_unit_ready(u) == 0) break;
        uint8_t sense[18];
        scsi_request_sense(u, sense);
        timer_sleep(100);
    }

    uint8_t inq[36];
    for (int i = 0; i < 36; i++) inq[i] = 0;
    scsi_inquiry(u, inq);

    if (scsi_read_capacity(u) != 0) {
        klog("USBSTOR", SEV_WARN, "READ CAPACITY failed - device not registered");
        u->in_use = 0;
        return 0;
    }

    /* The disk layer counts 512-byte sectors. A device with 4096-byte blocks
       reports a quarter as many, so convert rather than handing over the raw
       block count. */
    uint64_t sectors = u->blocks << u->sector_shift;

    /* INQUIRY bytes 8..31 are vendor and product, space-padded ASCII. */
    char model[44];
    int m = 0;
    for (int i = 8; i < 32 && m < 42; i++) {
        char c = (char)inq[i];
        if (c < 0x20 || c > 0x7E) c = ' ';
        model[m++] = c;
    }
    while (m > 0 && model[m - 1] == ' ') m--;      /* trim the padding */
    model[m] = 0;
    if (m == 0) { model[0] = 'U'; model[1] = 'S'; model[2] = 'B'; model[3] = 0; }

    int id = disk_register(DISK_DRV_USB, unit_index, DISK_MEDIA_GENERIC,
                           DISK_ATTACH_USB, model, sectors);
    if (id < 0) { u->in_use = 0; return 0; }
    u->disk_id = (uint8_t)id;

    klog_u32("USBSTOR", SEV_OK, "registered disk, sectors: ", (uint32_t)sectors,
             LOG_COLOR_VALUE, "");
    klog_child_u32("  block size ", u->block_size, LOG_COLOR_VALUE, "");
    klog_child(model);
    return 1;
}

/* ---- block I/O ----------------------------------------------------------
 * Split into chunks a single bulk transfer can carry. The controller drivers
 * cap a transfer at their DMA buffer, so a large read has to be issued as
 * several commands rather than one. */
static int rw(uint8_t unit, uint64_t lba, uint32_t count, void *buf, int read) {
    if (unit >= MAX_UNITS || !units[unit].in_use) return DISK_ERR_NO_DEVICE;
    if (!buf || count == 0) return DISK_ERR_PARAMS;

    struct storage_unit *u = &units[unit];
    uint8_t sh = u->sector_shift;                  /* 512-byte sectors per block */

    if (lba + count > (u->blocks << sh)) return DISK_ERR_BOUNDS;

    /* A partial device block cannot be read or written on its own, so a request
       that is not block-aligned would need a read-modify-write the caller does
       not expect. Every device seen so far has 512-byte blocks and shift 0;
       refuse rather than corrupt if that changes. */
    if (sh && ((lba & ((1u << sh) - 1)) || (count & ((1u << sh) - 1))))
        return DISK_ERR_PARAMS;

    uint32_t per_xfer = USB_BLOCK_MAX / u->block_size;
    if (per_xfer == 0) per_xfer = 1;

    uint8_t *p = (uint8_t *)buf;
    uint64_t dev_lba = lba >> sh;
    uint32_t remaining_blocks = count >> sh;
    if (remaining_blocks == 0) remaining_blocks = 1;

    while (remaining_blocks) {
        uint32_t n = remaining_blocks > per_xfer ? per_xfer : remaining_blocks;
        if (scsi_rw10(u, dev_lba, (uint16_t)n, p, read) < 0) return DISK_ERR_FAULT;
        p += n * u->block_size;
        dev_lba += n;
        remaining_blocks -= n;
    }
    return DISK_OK;
}

int usb_storage_read(uint8_t unit, uint64_t lba, uint32_t count, void *buf) {
    return rw(unit, lba, count, buf, 1);
}

int usb_storage_write(uint8_t unit, uint64_t lba, uint32_t count, const void *buf) {
    return rw(unit, lba, count, (void *)buf, 0);
}
