// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/ahci.h */
/* Aurora Tejeda */
/*
 * AHCI (Advanced Host Controller Interface) SATA driver.
 *
 * Finds the AHCI controller via PCI, maps its registers (ABAR/BAR5), probes
 * the ports for attached SATA disks, and does DMA sector reads/writes. This is
 * what lets the kernel see SATA disks on a modern chipset (the legacy PIO ata.c
 * only sees the old IDE ports).
 *
 * Up to AHCI_MAX_PORTS disks are exposed by index (port number).
 */

#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

#define AHCI_MAX_PORTS 32

/* probe PCI for an AHCI controller, init it, and detect disks on its ports.
   returns the number of SATA disks found (0 if no controller / no disks). */
int ahci_init(void);

/* is there a SATA disk on this port? */
int ahci_present(int port);

/* total sectors on the disk at `port` (from IDENTIFY); 0 if absent. */
uint64_t ahci_sectors(int port);

/* model string for the disk at `port` ("" if absent). */
const char *ahci_model(int port);
int ahci_is_ssd(int port);   /* 1 if solid-state */

/* read `count` sectors starting at `lba` into `buf` (count*512 bytes).
   returns 0 on success, -1 on error. */
int ahci_read(int port, uint64_t lba, uint32_t count, void *buf);

/* write `count` sectors from `buf` to the disk at `port` starting at `lba`.
   returns 0 on success, -1 on error. */
int ahci_write(int port, uint64_t lba, uint32_t count, const void *buf);

/* FLUSH CACHE EXT: commit the drive's write cache. This backend had none, so
   an AHCI write was acknowledged from cache and a power cut lost it. */
int ahci_flush(int port);

#endif