// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/ata.h */
/* Aurora Tejeda */
/*
 * ATA PIO disk driver (28-bit LBA, programmed I/O).
 *
 * Talks to the legacy ATA/IDE ports directly. Works when the controller is
 * in IDE/legacy mode (the emulator default). Reads/writes 512-byte sectors.
 *
 * Drive select: 0 = primary master, 1 = primary slave.
 * (Boot disk = master, filesystem disk = slave in our config.)
 */

#ifndef ATA_H
#define ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

/* probe the drives; call once at boot. */
void ata_init(void);

/* read `count` sectors starting at LBA `lba` from `drive` into `buffer`.
   buffer must be at least count*512 bytes. returns 0 on success, -1 on error. */
int ata_read(uint8_t drive, uint32_t lba, uint8_t count, void *buffer);

/* write `count` sectors starting at LBA `lba` to `drive` from `buffer`.
   returns 0 on success, -1 on error. */
int ata_write(uint8_t drive, uint32_t lba, uint8_t count, const void *buffer);

/* is the given drive present? (set by ata_init) */
int ata_present(uint8_t drive);

/* IDENTIFY info captured at probe time (valid only if ata_present(drive)). */
const char *ata_model(uint8_t drive);    /* model string ("" if absent) */
uint32_t    ata_sectors(uint8_t drive);
int         ata_is_ssd(uint8_t drive);   /* 1 if solid-state (rotation rate=1) */  /* total 28-bit LBA sectors (0 if absent) */

#endif