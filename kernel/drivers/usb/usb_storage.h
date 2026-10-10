// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/usb/usb_storage.h */
/* Aurora Tejeda / CATX Systems */
/*
 * USB mass storage - Bulk-Only Transport carrying SCSI.
 *
 * Controller-independent by construction: it talks to usb_hc_ops.bulk and does
 * not know or care whether that is EHCI, OHCI or xHCI underneath. A flash drive
 * on any of them registers as an ordinary disk.
 *
 * ---- The protocol, which is simpler than it sounds -----------------------
 *
 * Every command is three bulk transfers:
 *
 *   1. a 31-byte Command Block Wrapper OUT, carrying a SCSI command
 *   2. the data, IN or OUT, if the command has any
 *   3. a 13-byte Command Status Wrapper IN
 *
 * The tag in the CSW must match the one in the CBW, which is the only thing
 * keeping host and device in step if a transfer is ever lost.
 */

#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include <stdint.h>

struct usb_device;

/* Claim a device the core has enumerated, if it is bulk-only mass storage.
   Reads its capacity and registers it with the disk layer. Returns 1 if the
   device was claimed. */
int usb_storage_attach(struct usb_device *dev);

/* Block I/O, called through the disk layer's dispatch. `unit` is the index the
   driver handed disk_register(). Returns a disk_err code. */
int usb_storage_read(uint8_t unit, uint64_t lba, uint32_t count, void *buf);
int usb_storage_write(uint8_t unit, uint64_t lba, uint32_t count, const void *buf);

#endif
