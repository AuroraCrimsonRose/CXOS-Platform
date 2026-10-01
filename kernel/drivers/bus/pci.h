/* /kernel/drivers/bus/pci.h */
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

/* Cap on enumerated devices. 64 was fine against QEMU's half-dozen and far too
   small for real hardware: an SB950 alone presents five OHCI and four EHCI
   functions, plus SATA, SMBus, IDE, HD audio, LPC and a PCI bridge, and a K10
   adds five HyperTransport functions of its own - before anything behind the
   PCIe root ports is counted. Overflowing it made a device simply not exist,
   with no message, which presents as a driver bug rather than a full table. */
#define PCI_MAX_DEVICES 256

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

/* ---- BARs ---------------------------------------------------------------
 * Decoded rather than raw. Every driver was masking bar[n] by hand, and the
 * 64-bit form - where a BAR pairs with the next one to make a single address -
 * had been open-coded in exactly one of them. */
struct pci_bar {
    uint64_t base;       /* decoded base address                              */
    uint32_t size;       /* bytes, 0 if it could not be probed                */
    uint8_t  is_io;      /* 1 = I/O port space, 0 = memory                    */
    uint8_t  is_64;      /* occupies this BAR and the next                    */
    uint8_t  prefetch;
    uint8_t  valid;      /* 0 = unimplemented BAR                             */
};

/* Decode BAR `index` (0..5). Returns 1 if the BAR is implemented. Probes the
   size, which briefly disables the device's decode - safe at init, not while
   the device is running. */
int pci_bar_get(const struct pci_device *d, int index, struct pci_bar *out);

/* The common case: a 32-bit-addressable MMIO base this kernel can identity-map.
   Returns 0, having logged why, if the BAR is I/O space, lives above 4 GB, or
   lands below 0xC0000000.
 *
 * That last rule is CXK's, not PCI's: register windows are identity-mapped, and
 * only the kernel half of the address space is shared by every process. A BAR
 * below it would be mapped into the per-process half and fault the moment a
 * driver touched it from a syscall. */
uint32_t pci_bar_mmio32(const struct pci_device *d, int index, const char *tag);

/* Set the Bus Master and Memory Space enable bits. Without bus master the
   device never reads a descriptor ring, which looks like a dead controller. */
void pci_enable_bus_master(const struct pci_device *d);

/* ---- capabilities -------------------------------------------------------
 * The capability list is a linked list in config space, headed at offset 0x34
 * and only present when the status register says so. Walk it to find MSI,
 * MSI-X, PCIe, or EHCI's USB legacy-support capability.
 *
 * Returns the config-space offset of the first capability with this id, or 0. */
uint8_t pci_find_capability(const struct pci_device *d, uint8_t cap_id);

/* ---- MSI / MSI-X --------------------------------------------------------
 * A device signals an MSI by WRITING A DWORD TO MEMORY at the address the host
 * gives it - 0xFEE..... , the local APIC. There is no interrupt line involved,
 * which is why this needs no I/O APIC routing and no free IRQ pin, and why it
 * cannot work at all on a machine whose local APIC is not enabled.
 *
 * MSI-X is the same idea with the vectors in a table in device memory instead
 * of in config space, so a device can have many of them independently masked.
 *
 * `vector` is an IDT vector, not an IRQ number. Returns 1 on success. */
int pci_msi_enable(const struct pci_device *d, uint8_t vector);
int pci_msix_enable(const struct pci_device *d, uint8_t vector);

/* Enable whichever the device supports, preferring MSI-X. Returns 1 if either
   was configured, 0 if the device has neither or the local APIC is off. */
int pci_msi_setup(const struct pci_device *d, uint8_t vector);

/* Turn MSI/MSI-X back off, so the device falls back to its pin. */
void pci_msi_disable(const struct pci_device *d);

#define PCI_CAP_ID_MSI   0x05
#define PCI_CAP_ID_VNDR  0x09
#define PCI_CAP_ID_MSIX  0x11
#define PCI_CAP_ID_PCIE  0x10

/* raw config-space access (32-bit, offset must be dword-aligned). */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t off, uint32_t value);

/* convenience narrower reads */
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);

#endif