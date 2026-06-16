/* /CXLite/kernel/drivers/pci.c */
/* Aurora Tejeda */
/* PCI enumeration via 0xCF8/0xCFC config mechanism. */

#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_MAX_DEVICES 64
static struct pci_device devices[PCI_MAX_DEVICES];
static unsigned dev_count = 0;

/* build the 0xCF8 address: enable bit | bus | slot | func | (offset & 0xFC) */
static uint32_t cfg_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint32_t)0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func << 8)
         | ((uint32_t)off & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDR, cfg_address(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_config_read32(bus, slot, func, off & 0xFC);
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_config_read32(bus, slot, func, off & 0xFC);
    return (uint8_t)((v >> ((off & 3) * 8)) & 0xFF);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    outl(PCI_CONFIG_ADDR, cfg_address(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, val);
}

/* read one function's identity; returns 0 if no device present here */
static int read_function(uint8_t bus, uint8_t slot, uint8_t func, struct pci_device *d) {
    uint16_t vendor = pci_config_read16(bus, slot, func, 0x00);
    if (vendor == 0xFFFF) return 0;   /* no device */

    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor_id = vendor;
    d->device_id = pci_config_read16(bus, slot, func, 0x02);

    uint32_t classreg = pci_config_read32(bus, slot, func, 0x08);
    d->prog_if    = (uint8_t)((classreg >> 8)  & 0xFF);
    d->subclass   = (uint8_t)((classreg >> 16) & 0xFF);
    d->class_code = (uint8_t)((classreg >> 24) & 0xFF);

    d->header_type = pci_config_read8(bus, slot, func, 0x0E);

    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_config_read32(bus, slot, func, 0x10 + i * 4);

    return 1;
}

void pci_init(void) {
    dev_count = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            /* function 0 must exist for the slot to be populated */
            uint16_t vendor = pci_config_read16((uint8_t)bus, slot, 0, 0x00);
            if (vendor == 0xFFFF) continue;

            /* if header type bit 7 set, it's multi-function: probe 0..7 */
            uint8_t htype = pci_config_read8((uint8_t)bus, slot, 0, 0x0E);
            uint8_t nfunc = (htype & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < nfunc; func++) {
                if (dev_count >= PCI_MAX_DEVICES) return;
                struct pci_device d;
                if (read_function((uint8_t)bus, slot, func, &d))
                    devices[dev_count++] = d;
            }
        }
    }
}

unsigned pci_device_count(void) { return dev_count; }

const struct pci_device *pci_get_device(unsigned i) {
    if (i >= dev_count) return 0;
    return &devices[i];
}

const struct pci_device *pci_find(uint8_t class_code, uint8_t subclass,
                                  int prog_if, unsigned index) {
    unsigned seen = 0;
    for (unsigned i = 0; i < dev_count; i++) {
        const struct pci_device *d = &devices[i];
        if (d->class_code == class_code && d->subclass == subclass &&
            (prog_if < 0 || d->prog_if == (uint8_t)prog_if)) {
            if (seen == index) return d;
            seen++;
        }
    }
    return 0;
}

/* decode the most relevant class/subclass/prog_if combinations into names.
   Not exhaustive - covers what we care about plus generic fallbacks. */
const char *pci_class_name(uint8_t cc, uint8_t sc, uint8_t pif) {
    switch (cc) {
        case 0x00: return "Unclassified";
        case 0x01: /* Mass Storage */
            switch (sc) {
                case 0x01: return "Storage: IDE";
                case 0x05: return "Storage: ATA";
                case 0x06:
                    if (pif == 0x01) return "Storage: SATA [AHCI]";
                    return "Storage: SATA";
                case 0x08: return "Storage: NVMe";
                default:   return "Storage: other";
            }
        case 0x02: /* Network */
            switch (sc) {
                case 0x00: return "Network: Ethernet";
                case 0x80: return "Network: other";
                default:   return "Network: controller";
            }
        case 0x03: /* Display */
            switch (sc) {
                case 0x00: return "Display: VGA";
                case 0x02: return "Display: 3D";
                default:   return "Display: controller";
            }
        case 0x04: return "Multimedia";
        case 0x06: /* Bridge */
            switch (sc) {
                case 0x00: return "Bridge: Host";
                case 0x01: return "Bridge: ISA";
                case 0x04: return "Bridge: PCI-to-PCI";
                default:   return "Bridge: other";
            }
        case 0x0C: /* Serial bus */
            switch (sc) {
                case 0x03: /* USB */
                    switch (pif) {
                        case 0x00: return "USB [UHCI]";
                        case 0x10: return "USB [OHCI]";
                        case 0x20: return "USB [EHCI]";
                        case 0x30: return "USB [xHCI]";
                        default:   return "USB [other]";
                    }
                case 0x05: return "SMBus";
                default:   return "Serial bus: other";
            }
        default: return "Device";
    }
}