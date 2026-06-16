/* /CXLite/kernel/drivers/pci.h */
/* Aurora Tejeda */
/*
 * PCI bus enumeration via the legacy 0xCF8/0xCFC config mechanism.
 *
 * Walks every bus/device/function, records what's present, and decodes class
 * codes into human-readable names. This is the foundation for finding the
 * AHCI controller, USB (OHCI/EHCI) controllers, the NIC, the GPU, etc. - any
 * future device driver starts by locating its controller here.
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* a discovered PCI function */
struct pci_device {
    uint8_t  bus;
    uint8_t  slot;        /* device number 0..31 */
    uint8_t  func;        /* function number 0..7 */
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;  /* high-level class (0x01=storage, 0x02=net, ...) */
    uint8_t  subclass;
    uint8_t  prog_if;     /* programming interface (distinguishes AHCI/OHCI/...) */
    uint8_t  header_type;
    uint32_t bar[6];      /* base address registers (raw) */
};

/* ---- raw config-space access ---- */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t  pci_config_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val);

/* ---- enumeration ---- */
void pci_init(void);                 /* scan all buses, fill the device table */
unsigned pci_device_count(void);
const struct pci_device *pci_get_device(unsigned i);

/* find the Nth device matching class/subclass (prog_if -1 = any).
   returns 0 if not found. used by drivers to locate their controller. */
const struct pci_device *pci_find(uint8_t class_code, uint8_t subclass,
                                  int prog_if, unsigned index);

/* human-readable names for a class/subclass/prog_if (for lspci output) */
const char *pci_class_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

#endif