/* /kernel/drivers/bus/pci.c */
/* Aurora Tejeda / CATX Systems LLC */
/* PCI bus driver - legacy configuration mechanism #1 (see pci.h). */

#include "pci.h"
#include "io.h"
#include "logging.h"
#include "apic.h"
#include "paging.h"

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
static int table_full_reported = 0;

static void add_function(uint8_t bus, uint8_t slot, uint8_t func) {
    if (ndevices >= PCI_MAX_DEVICES) {
        /* Say so once. Silently dropping devices here is indistinguishable
           from a driver failing to find its hardware. */
        if (!table_full_reported) {
            table_full_reported = 1;
            klog_u32("PCI", SEV_WARN, "device table full at ", PCI_MAX_DEVICES,
                     LOG_COLOR_VALUE, " - later devices are NOT enumerated");
        }
        return;
    }
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
    table_full_reported = 0;
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

/* Config space is written a DWORD at a time by the hardware interface, so a
   16-bit field has to be merged into its containing dword. Writing the dword
   with the other half zeroed would clear a neighbouring register. */
static void pci_config_write16_rmw(const struct pci_device *d, uint8_t off, uint16_t v) {
    uint32_t dw = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(off & 0xFC));
    unsigned shift = (off & 2) * 8;
    dw &= ~(0xFFFFu << shift);
    dw |= ((uint32_t)v << shift);
    pci_config_write32(d->bus, d->slot, d->func, (uint8_t)(off & 0xFC), dw);
}

/* ---- command register ---------------------------------------------------- */
void pci_enable_bus_master(const struct pci_device *d) {
    if (!d) return;
    uint32_t cmd = pci_config_read32(d->bus, d->slot, d->func, 0x04);
    cmd |= (1u << 1) | (1u << 2);          /* memory space | bus master */
    pci_config_write32(d->bus, d->slot, d->func, 0x04, cmd);
}

/* ---- BARs ---------------------------------------------------------------- */
int pci_bar_get(const struct pci_device *d, int index, struct pci_bar *out) {
    if (!d || !out || index < 0 || index > 5) return 0;
    for (unsigned i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

    uint8_t off = (uint8_t)(0x10 + index * 4);
    uint32_t raw = pci_config_read32(d->bus, d->slot, d->func, off);
    if (raw == 0) return 0;                /* unimplemented */

    out->is_io = (uint8_t)(raw & 1u);
    if (out->is_io) {
        out->base = raw & 0xFFFFFFFCu;
    } else {
        out->prefetch = (uint8_t)((raw >> 3) & 1u);
        /* type in bits 2:1 - 0b10 means this BAR and the NEXT one together
           form a single 64-bit address. */
        out->is_64 = (uint8_t)(((raw >> 1) & 3u) == 2u);
        out->base = raw & 0xFFFFFFF0u;
        if (out->is_64 && index < 5) {
            uint32_t hi = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(off + 4));
            out->base |= (uint64_t)hi << 32;
        }
    }

    /* Size probe: write all ones, read back the mask the device leaves, then
       restore. The device must not be decoding while its BAR holds a bogus
       address, so turn memory and I/O decode off across the probe. */
    uint32_t cmd = pci_config_read32(d->bus, d->slot, d->func, 0x04);
    pci_config_write32(d->bus, d->slot, d->func, 0x04, cmd & ~0x3u);

    pci_config_write32(d->bus, d->slot, d->func, off, 0xFFFFFFFFu);
    uint32_t mask = pci_config_read32(d->bus, d->slot, d->func, off);
    pci_config_write32(d->bus, d->slot, d->func, off, raw);

    pci_config_write32(d->bus, d->slot, d->func, 0x04, cmd);

    mask &= out->is_io ? 0xFFFFFFFCu : 0xFFFFFFF0u;
    out->size = mask ? (~mask + 1u) : 0u;

    out->valid = 1;
    return 1;
}

uint32_t pci_bar_mmio32(const struct pci_device *d, int index, const char *tag) {
    struct pci_bar b;
    if (!pci_bar_get(d, index, &b)) return 0;

    if (b.is_io) {
        klog_u32(tag, SEV_WARN, "BAR ", (uint32_t)index, LOG_COLOR_VALUE,
                 " is I/O space, not memory");
        return 0;
    }
    if (b.base >> 32) {
        klog_u32(tag, SEV_WARN, "BAR ", (uint32_t)index, LOG_COLOR_VALUE,
                 " is above 4 GB and unreachable from a 32-bit kernel");
        return 0;
    }
    uint32_t base = (uint32_t)b.base;
    if (base == 0) return 0;
    if (base < 0xC0000000u) {
        klog_u32(tag, SEV_WARN, "register BAR below the kernel half: ", base,
                 LOG_COLOR_VALUE, " - not visible from a process address space");
        return 0;
    }
    return base;
}

/* ---- capabilities -------------------------------------------------------- */
uint8_t pci_find_capability(const struct pci_device *d, uint8_t cap_id) {
    if (!d) return 0;

    uint16_t status = pci_config_read16(d->bus, d->slot, d->func, 0x06);
    if (!(status & (1u << 4))) return 0;      /* no capability list */

    uint8_t off = pci_config_read8(d->bus, d->slot, d->func, 0x34) & 0xFC;
    /* Bounded: a malformed or circular list would otherwise spin forever, and
       the list cannot legally exceed 48 entries in 256 bytes of config space. */
    for (int guard = 0; guard < 48 && off >= 0x40; guard++) {
        uint8_t id   = pci_config_read8(d->bus, d->slot, d->func, off);
        uint8_t next = pci_config_read8(d->bus, d->slot, d->func, (uint8_t)(off + 1));
        if (id == cap_id) return off;
        if (next == 0) break;
        off = next & 0xFC;
    }
    return 0;
}

/* ---- MSI ----------------------------------------------------------------
 * The capability holds a message control word, an address, and a data value.
 * The device writes `data` to `address` when it wants attention; the local APIC
 * turns that write into the vector encoded in the data. Both values are
 * architectural on x86 rather than device-specific, which is why they come from
 * the APIC code rather than from here. */
#define MSI_CTRL_ENABLE      (1u << 0)
#define MSI_CTRL_64BIT       (1u << 7)
#define MSI_CTRL_MULTI_MASK  (7u << 4)

int pci_msi_enable(const struct pci_device *d, uint8_t vector) {
    if (!d || !lapic_active()) return 0;
    uint8_t cap = pci_find_capability(d, PCI_CAP_ID_MSI);
    if (!cap) return 0;

    uint16_t ctrl = pci_config_read16(d->bus, d->slot, d->func, (uint8_t)(cap + 2));

    pci_config_write32(d->bus, d->slot, d->func, (uint8_t)(cap + 4),
                       msi_message_address());

    /* Where the data word sits depends on whether the device is 64-bit capable:
       a 64-bit device has an upper-address dword in between. Writing data to
       the 32-bit offset on a 64-bit device puts it in the upper address
       instead, and the interrupt is then delivered nowhere. */
    if (ctrl & MSI_CTRL_64BIT) {
        pci_config_write32(d->bus, d->slot, d->func, (uint8_t)(cap + 8), 0);
        pci_config_write32(d->bus, d->slot, d->func, (uint8_t)(cap + 12),
                           msi_message_data(vector));
    } else {
        pci_config_write32(d->bus, d->slot, d->func, (uint8_t)(cap + 8),
                           msi_message_data(vector));
    }

    /* Request exactly one vector. The multiple-message field is a LOG2 count,
       so leaving whatever the device advertised would have it use several
       consecutive vectors we have not set up handlers for. */
    ctrl &= (uint16_t)~MSI_CTRL_MULTI_MASK;
    ctrl |= MSI_CTRL_ENABLE;
    pci_config_write16_rmw(d, (uint8_t)(cap + 2), ctrl);
    return 1;
}

/* ---- MSI-X --------------------------------------------------------------
 * The vectors live in a table in one of the device's BARs, not in config
 * space. The capability says which BAR and at what offset. */
#define MSIX_CTRL_ENABLE     (1u << 15)
#define MSIX_CTRL_FUNC_MASK  (1u << 14)
#define MSIX_CTRL_SIZE_MASK  0x7FF
#define MSIX_ENTRY_SIZE      16
#define MSIX_VEC_CTRL_MASK   1u

int pci_msix_enable(const struct pci_device *d, uint8_t vector) {
    if (!d || !lapic_active()) return 0;
    uint8_t cap = pci_find_capability(d, PCI_CAP_ID_MSIX);
    if (!cap) return 0;

    uint16_t ctrl = pci_config_read16(d->bus, d->slot, d->func, (uint8_t)(cap + 2));
    uint32_t tbl  = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(cap + 4));

    /* Low three bits are the BAR index, the rest a byte offset into it. */
    int bir = (int)(tbl & 0x7);
    uint32_t offset = tbl & ~0x7u;

    uint32_t base = pci_bar_mmio32(d, bir, "MSI-X");
    if (!base) return 0;

    uint32_t entry = base + offset;   /* entry 0 - one vector is all we want */
    paging_map_kernel(entry & ~0xFFFu, entry & ~0xFFFu,
               PAGE_PRESENT | PAGE_WRITE | PAGE_NO_CACHE);

    volatile uint32_t *e = (volatile uint32_t *)entry;
    e[0] = msi_message_address();     /* address low  */
    e[1] = 0;                         /* address high */
    e[2] = msi_message_data(vector);
    e[3] = 0;                         /* vector control: bit 0 clear = unmasked */

    /* Function mask must be cleared as well as the per-vector mask above - it
       gates the whole table and a device powers up with it set. */
    ctrl &= (uint16_t)~MSIX_CTRL_FUNC_MASK;
    ctrl |= MSIX_CTRL_ENABLE;
    pci_config_write16_rmw(d, (uint8_t)(cap + 2), ctrl);
    return 1;
}

int pci_msi_setup(const struct pci_device *d, uint8_t vector) {
    if (pci_msix_enable(d, vector)) return 1;
    return pci_msi_enable(d, vector);
}

void pci_msi_disable(const struct pci_device *d) {
    if (!d) return;
    uint8_t cap = pci_find_capability(d, PCI_CAP_ID_MSI);
    if (cap) {
        uint16_t ctrl = pci_config_read16(d->bus, d->slot, d->func, (uint8_t)(cap + 2));
        pci_config_write16_rmw(d, (uint8_t)(cap + 2),
                               (uint16_t)(ctrl & ~MSI_CTRL_ENABLE));
    }
    cap = pci_find_capability(d, PCI_CAP_ID_MSIX);
    if (cap) {
        uint16_t ctrl = pci_config_read16(d->bus, d->slot, d->func, (uint8_t)(cap + 2));
        pci_config_write16_rmw(d, (uint8_t)(cap + 2),
                               (uint16_t)(ctrl & ~MSIX_CTRL_ENABLE));
    }
}
