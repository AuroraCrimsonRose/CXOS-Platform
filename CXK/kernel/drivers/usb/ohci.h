/* /CXLite/kernel/drivers/ohci.h */
/* Aurora Tejeda */
/*
 * OHCI (Open Host Controller Interface) USB driver - STAGE 1.
 *
 * This stage brings up the OHCI controller and detects port connections. It
 * does NOT yet enumerate devices or do transfers (those are later stages).
 * Goal of stage 1: find the controller, initialize it, and report which root
 * ports have a device plugged in.
 *
 * OHCI is the USB 1.x host controller (low/full speed) - the right target for
 * simple "treat everything as USB 1" support. Its registers are memory-mapped
 * (via PCI BAR0), unlike UHCI which is port-mapped.
 */

#ifndef OHCI_H
#define OHCI_H

#include <stdint.h>

/* probe PCI for an OHCI controller and initialize it. returns the number of
   root-hub ports (0 if no controller found / init failed). */
int ohci_init(void);

int ohci_present(void);            /* was a controller found + initialized? */
int ohci_port_count(void);         /* number of root-hub downstream ports */
int ohci_port_connected(int port); /* 1 if a device is plugged into `port` */

/* Stage 2: enumerate the device on `port` (reset, read descriptor, set address).
   returns 1 on success; fills the device-info accessors below. */
int ohci_enumerate(int port);

int      ohci_dev_valid(void);     /* 1 if the last enumerate succeeded */
uint16_t ohci_dev_vendor(void);
uint16_t ohci_dev_product(void);
uint8_t  ohci_dev_class(void);     /* USB device class (0x08 = mass storage) */
uint8_t  ohci_dev_subclass(void);
uint8_t  ohci_dev_protocol(void);

/* interface class + bulk endpoints (from the config descriptor) - stage 3 uses these */
int      ohci_dev_if_valid(void);
uint8_t  ohci_dev_if_class(void);    /* 0x08 = mass storage */
uint8_t  ohci_dev_if_subclass(void); /* 0x06 = SCSI transparent */
uint8_t  ohci_dev_if_protocol(void); /* 0x50 = Bulk-Only Transport */
uint8_t  ohci_dev_ep_in(void);       /* bulk IN endpoint address */
uint8_t  ohci_dev_ep_out(void);      /* bulk OUT endpoint address */
uint8_t  ohci_dev_config_value(void);
uint16_t ohci_dev_ep_out_mps(void);

/* Stage 3: USB mass storage (Bulk-Only Transport + SCSI). */
int      ohci_storage_init(void);      /* INQUIRY + READ CAPACITY; 1 if ready */
int      ohci_storage_ready(void);
uint64_t ohci_storage_sectors(void);   /* capacity in blocks */
uint32_t ohci_storage_block_size(void);
uint8_t  ohci_storage_pdt(void);       /* SCSI peripheral device type */
int      ohci_storage_read(uint64_t lba, uint32_t count, void *buf);
int      ohci_storage_write(uint64_t lba, uint32_t count, const void *buf);
int      ohci_storage_step(void);
int      ohci_bulk_fail(void);      /* 1=halted, 2=timeout */
uint32_t ohci_bulk_cc(void);        /* last TD condition code */

#endif