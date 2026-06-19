/* /CXK/kernel/drivers/bus/pci.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * PCI bus driver (legacy configuration mechanism #1: ports 0xCF8/0xCFC).
 *
 * Enumerates the PCI bus at init, caching each device's identity and BARs, and
 * provides config-space read/write so device drivers (AHCI, USB, NIC) can find
 * and program their controllers. Pure port I/O - no DMA, no MMIO, no higher-half
 * concerns. This is a *bus* driver: it discovers the devices other drivers own.
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES 64       /* cap on enumerated devices */

struct pci_device {
    uint8_t  bus;
    uint8_t  slot;               /* device number (0..31) */
    uint8_t  func;               /* function number (0..7) */
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;         /* base class (e.g. 0x01 = mass storage) */
    uint8_t  subclass;           /* e.g. 0x06 = SATA */
    uint8_t  prog_if;            /* e.g. 0x01 = AHCI */
    uint8_t  header_type;
    uint32_t bar[6];             /* base address registers (raw) */
};

/* enumerate the PCI bus; fills the internal device table. Call once at boot. */
void pci_init(void);

/* number of devices found by pci_init(). */
unsigned pci_device_count(void);

/* get device by enumeration index (0 .. pci_device_count()-1), or 0. */
const struct pci_device *pci_get(unsigned index);

/* find the `index`-th device matching (class, subclass, prog_if). Pass prog_if
   0xFF to match any prog-if. Returns 0 if not found. */
const struct pci_device *pci_find(uint8_t class_code, uint8_t subclass,
                                  uint8_t prog_if, unsigned index);

/* raw config-space access (32-bit, offset must be dword-aligned). */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t off, uint32_t value);

/* convenience narrower reads */
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);

#endif