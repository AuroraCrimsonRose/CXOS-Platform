/* /CXK/kernel/drivers/bus/pci.c */
/* Aurora Tejeda / CATX Systems LLC */
/* PCI bus driver - legacy configuration mechanism #1 (see pci.h). */

#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

/* build the config address dword: enable bit (31) | bus | slot | func | offset.
   offset is dword-aligned (low 2 bits cleared). */
static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint32_t)((1u << 31)
                    | ((uint32_t)bus  << 16)
                    | ((uint32_t)slot << 11)
                    | ((uint32_t)func << 8)
                    | ((uint32_t)off & 0xFC));
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t off, uint32_t value) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_config_read32(bus, slot, func, off & 0xFC);
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_config_read32(bus, slot, func, off & 0xFC);
    return (uint8_t)((v >> ((off & 3) * 8)) & 0xFF);
}

/* enumerated device table */
static struct pci_device devices[PCI_MAX_DEVICES];
static unsigned ndevices = 0;

static uint16_t read_vendor(uint8_t bus, uint8_t slot, uint8_t func) {
    return pci_config_read16(bus, slot, func, 0x00);
}

/* read one function's identity + BARs into the table (caller checked it exists). */
static void add_function(uint8_t bus, uint8_t slot, uint8_t func) {
    if (ndevices >= PCI_MAX_DEVICES) return;
    struct pci_device *d = &devices[ndevices];

    d->bus  = bus;
    d->slot = slot;
    d->func = func;

    uint32_t id  = pci_config_read32(bus, slot, func, 0x00);
    d->vendor_id = (uint16_t)(id & 0xFFFF);
    d->device_id = (uint16_t)(id >> 16);

    uint32_t cls = pci_config_read32(bus, slot, func, 0x08);
    d->prog_if    = (uint8_t)((cls >> 8)  & 0xFF);
    d->subclass   = (uint8_t)((cls >> 16) & 0xFF);
    d->class_code = (uint8_t)((cls >> 24) & 0xFF);

    d->header_type = pci_config_read8(bus, slot, func, 0x0E);

    /* BARs live at config offsets 0x10..0x24 (6 dwords). Only header type 0
       (general device) has all six; type 1 (PCI-to-PCI bridge) has two. Read
       six regardless; bridges just get junk in the upper BARs (harmless, we
       only consume bar[5] for AHCI on a type-0 device). */
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_config_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));

    ndevices++;
}

static void scan_slot(uint8_t bus, uint8_t slot) {
    if (read_vendor(bus, slot, 0) == 0xFFFF) return;   /* no device in slot */

    add_function(bus, slot, 0);

    /* if multi-function (header type bit 7 set), probe functions 1..7 */
    uint8_t htype = pci_config_read8(bus, slot, 0, 0x0E);
    if (htype & 0x80) {
        for (uint8_t func = 1; func < 8; func++)
            if (read_vendor(bus, slot, func) != 0xFFFF)
                add_function(bus, slot, func);
    }
}

void pci_init(void) {
    ndevices = 0;
    /* brute-force scan: all 256 buses x 32 slots. Simple and reliable; the
       recursive bus-discovery optimization isn't worth it for our scale. */
    for (unsigned bus = 0; bus < 256; bus++)
        for (uint8_t slot = 0; slot < 32; slot++)
            scan_slot((uint8_t)bus, slot);
}

unsigned pci_device_count(void) { return ndevices; }

const struct pci_device *pci_get(unsigned index) {
    return (index < ndevices) ? &devices[index] : 0;
}

const struct pci_device *pci_find(uint8_t class_code, uint8_t subclass,
                                  uint8_t prog_if, unsigned index) {
    unsigned match = 0;
    for (unsigned i = 0; i < ndevices; i++) {
        struct pci_device *d = &devices[i];
        if (d->class_code == class_code && d->subclass == subclass &&
            (prog_if == 0xFF || d->prog_if == prog_if)) {
            if (match == index) return d;
            match++;
        }
    }
    return 0;
}