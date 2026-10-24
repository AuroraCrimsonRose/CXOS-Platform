// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/ahci.c */
/* Aurora Tejeda */
/* AHCI SATA driver - PCI discovery, port init, DMA read/write. */

#include "ahci.h"
#include "pci.h"
#include "arch/paging.h"
#include "pmm.h"
#include "timer.h"
#include "console.h"
#include "string.h"
#include "disk.h"
#include "logging.h"
#include "drivers/iowin.h"

/* ---- HBA memory registers (the ABAR-mapped structure) ---- */

/* generic host control (at ABAR + 0x00) */
#define HBA_GHC          0x04   /* global host control */
#define HBA_PI           0x0C   /* ports implemented (bitmask) */
#define HBA_CAP2         0x24
#define HBA_BOHC         0x28   /* BIOS/OS handoff */

/* per-port registers (at ABAR + 0x100 + port*0x80) */
#define HBA_PORT(p)      (0x100u + (uint32_t)(p) * 0x80u)
#define PX_CLB           0x00   /* command list base (1K aligned) */
#define PX_CLBU          0x04   /* command list base upper 32 (we keep 0) */
#define PX_FB            0x08   /* FIS base (256 aligned) */
#define PX_FBU           0x0C   /* FIS base upper 32 */
#define PX_IS            0x10   /* interrupt status */
#define PX_CMD           0x18   /* command and status */
#define PX_TFD           0x20   /* task file data */
#define PX_SIG           0x24   /* signature */
#define PX_SSTS          0x28   /* SATA status (SCR0) */
#define PX_CI            0x38   /* command issue */

/* command header (entry in the command list; 32 bytes each, 32 per port) */
struct hba_cmd_header {
    uint8_t  cfl:5;      /* command FIS length in dwords */
    uint8_t  a:1;        /* ATAPI */
    uint8_t  w:1;        /* write (1) / read (0) */
    uint8_t  p:1;        /* prefetchable */
    uint8_t  r:1;        /* reset */
    uint8_t  b:1;        /* BIST */
    uint8_t  c:1;        /* clear busy on R_OK */
    uint8_t  rsv0:1;
    uint8_t  pmp:4;      /* port multiplier port */
    uint16_t prdtl;      /* PRDT entry count */
    volatile uint32_t prdbc; /* bytes transferred */
    uint32_t ctba;       /* command table base (128 aligned) */
    uint32_t ctbau;
    uint32_t reserved[4];
};

/* physical region descriptor (scatter-gather entry) */
struct hba_prdt_entry {
    uint32_t dba;        /* data base address */
    uint32_t dbau;
    uint32_t reserved0;
    uint32_t dbc:22;     /* byte count - 1 */
    uint32_t reserved1:9;
    uint32_t i:1;        /* interrupt on completion */
};

/* command table: a command FIS + (optional ATAPI) + PRDT entries */
struct hba_cmd_table {
    uint8_t  cfis[64];   /* command FIS */
    uint8_t  acmd[16];   /* ATAPI command */
    uint8_t  reserved[48];
    struct hba_prdt_entry prdt[8];  /* up to 8 PRDT entries (enough for us) */
};

/* register host-to-device FIS */
struct fis_reg_h2d {
    uint8_t  fis_type;   /* 0x27 */
    uint8_t  pmport:4;
    uint8_t  rsv0:3;
    uint8_t  c:1;        /* 1 = command */
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;
    uint8_t  lba3, lba4, lba5;
    uint8_t  featureh;
    uint8_t  countl, counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv1[4];
};

#define FIS_TYPE_REG_H2D 0x27
#define ATA_CMD_READ_DMA_EX   0x25
#define ATA_CMD_WRITE_DMA_EX  0x35
#define ATA_CMD_FLUSH_EX      0xEA   /* FLUSH CACHE EXT: no data transfer */
#define ATA_CMD_IDENTIFY      0xEC

#define HBA_PORT_CMD_ST   0x0001   /* start */
#define HBA_PORT_CMD_FRE  0x0010   /* FIS receive enable */
#define HBA_PORT_CMD_FR   0x4000   /* FIS receive running */
#define HBA_PORT_CMD_CR   0x8000   /* command list running */

#define HBA_PxIS_TFES     (1u << 30)  /* task file error */

#define SATA_SIG_ATA      0x00000101  /* SATA drive signature */

#define HBA_TFD_BSY       0x80
#define HBA_TFD_DRQ       0x08
#define HBA_TFD_ERR       0x01      /* task file error bit (status bit 0) */

/* wall-clock budget for AHCI command waits (ms). Generous for slow drives. */
#define AHCI_TIMEOUT_MS   3000

/* ---- driver state ---- */

static struct iowin hba;   /* ABAR: the generic registers, then every port's */
static int port_present[AHCI_MAX_PORTS];
static uint64_t port_sectors[AHCI_MAX_PORTS];
static char     port_model[AHCI_MAX_PORTS][41];
static uint8_t  port_ssd[AHCI_MAX_PORTS];
static int      ndisks = 0;

/* ---- v5 DMA memory --------------------------------------------------------
 * AHCI hardware does DMA: it reads/writes RAM by PHYSICAL address, ignoring
 * paging. The kernel, running higher-half, builds those structures via VIRTUAL
 * addresses. So every DMA structure needs BOTH a physical address (for the HBA
 * registers) and a kernel-virtual mapping (so we can fill it in). v4 assumed
 * identity mapping (virt == phys); v5 has none, so we:
 *
 *   - allocate the per-port command/FIS/table region from the PMM (physical),
 *     map it into a kernel-virtual window, and keep both bases. The HBA gets
 *     PHYS; the kernel writes via VIRT.
 *   - use a PMM-backed, physically-contiguous BOUNCE buffer for data transfers,
 *     so we never depend on a caller's buffer being physically contiguous
 *     (a virtually-contiguous multi-page buffer may not be physically so, which
 *     would corrupt a single-PRDT DMA). Reads: HBA -> bounce -> memcpy to caller.
 *     Writes: memcpy caller -> bounce -> HBA.
 *
 * DMA region is mapped at a dedicated kernel-virtual window. */
#define AHCI_PORT_STRIDE  0x1000u             /* one page of structures per port */
#define AHCI_MAX_PORTS    32
#define AHCI_DMA_VIRT     KVA_AHCI_DMA        /* kernel-virtual window for AHCI DMA
                                                 (arch/<arch>/paging_defs.h) */
#define AHCI_BOUNCE_PAGES 16                  /* 64 KB bounce: up to 128 sectors/xfer */

static uint32_t dma_phys_base = 0;            /* physical base of the structures region */
static uintptr_t dma_virt_base = 0;           /* kernel-virtual base of same */
static uint32_t bounce_phys   = 0;            /* physical base of the bounce buffer */
static uint8_t *bounce_virt   = 0;            /* kernel-virtual base of the bounce buffer */

/* per-port physical addresses (what the HBA registers want) */
static uint32_t port_clb_phys(int p)  { return dma_phys_base + p * AHCI_PORT_STRIDE + 0x000; }
static uint32_t port_fb_phys(int p)   { return dma_phys_base + p * AHCI_PORT_STRIDE + 0x400; }
static uint32_t port_ctba_phys(int p) { return dma_phys_base + p * AHCI_PORT_STRIDE + 0x800; }

/* per-port kernel-virtual addresses (what the kernel writes through) */
static void *port_clb_virt(int p)  { return (void *)(dma_virt_base + p * AHCI_PORT_STRIDE + 0x000); }
static void *port_fb_virt(int p)   { return (void *)(dma_virt_base + p * AHCI_PORT_STRIDE + 0x400); }
static void *port_ctba_virt(int p) { return (void *)(dma_virt_base + p * AHCI_PORT_STRIDE + 0x800); }

/* allocate + map the DMA structures region and the bounce buffer. Called once
   before any port is rebased. Returns 0 on success. */
static int ahci_dma_init(void) {
    /* structures: one page per port, physically contiguous from the PMM */
    paddr_t sp = pmm_alloc_pages(AHCI_MAX_PORTS);
    if (!sp) return -1;
    if ((uint64_t)sp >> 32) return -1;   /* this driver programs 32-bit DMA addresses */
    dma_phys_base = (uint32_t)sp;
    dma_virt_base = AHCI_DMA_VIRT;
    for (int i = 0; i < AHCI_MAX_PORTS; i++)
        paging_map_kernel(dma_virt_base + i * 0x1000,
                   dma_phys_base + i * 0x1000,
                   VM_WRITE);

    /* bounce buffer: physically contiguous, for data transfers */
    paddr_t bp = pmm_alloc_pages(AHCI_BOUNCE_PAGES);
    if (!bp || (uint64_t)bp >> 32) return -1;
    bounce_phys = (uint32_t)bp;
    bounce_virt = (uint8_t *)(uintptr_t)(AHCI_DMA_VIRT + AHCI_MAX_PORTS * 0x1000);
    for (int i = 0; i < AHCI_BOUNCE_PAGES; i++)
        paging_map_kernel((uintptr_t)bounce_virt + i * 0x1000,
                   bounce_phys + i * 0x1000,
                   VM_WRITE);
    return 0;
}

static uint32_t hba_rd(uint32_t reg)             { return iowin_read32(&hba, reg); }
static void     hba_wr(uint32_t reg, uint32_t v) { iowin_write32(&hba, reg, v); }
static uint32_t px_rd(int p, uint32_t reg)             { return hba_rd(HBA_PORT(p) + reg); }
static void     px_wr(int p, uint32_t reg, uint32_t v) { hba_wr(HBA_PORT(p) + reg, v); }

/* stop a port's command engine before reprogramming its pointers */
static void port_stop(int p) {
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) & ~HBA_PORT_CMD_ST);
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) & ~HBA_PORT_CMD_FRE);
    /* wait for FR and CR to clear */
    uint32_t timeout = 1000000;
    while ((px_rd(p, PX_CMD) & (HBA_PORT_CMD_FR | HBA_PORT_CMD_CR)) && timeout--) { }
}

/* start a port's command engine */
static void port_start(int p) {
    uint32_t timeout = 1000000;
    while ((px_rd(p, PX_CMD) & HBA_PORT_CMD_CR) && timeout--) { }
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | HBA_PORT_CMD_FRE);
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | HBA_PORT_CMD_ST);
}

/* set up a port's command list + FIS area in the DMA region */
static void port_rebase(int p) {
    port_stop(p);

    /* zero and assign the command list (32 headers * 32 bytes = 1KB). Kernel
       writes via VIRT; the HBA register gets PHYS. */
    uint8_t *clb = (uint8_t *)port_clb_virt(p);
    for (int i = 0; i < 1024; i++) clb[i] = 0;
    px_wr(p, PX_CLB, port_clb_phys(p));
    px_wr(p, PX_CLBU, 0);

    /* zero and assign the FIS receive area (256 bytes) */
    uint8_t *fb = (uint8_t *)port_fb_virt(p);
    for (int i = 0; i < 256; i++) fb[i] = 0;
    px_wr(p, PX_FB, port_fb_phys(p));
    px_wr(p, PX_FBU, 0);

    /* point command header 0 at our single command table (HBA wants PHYS) */
    struct hba_cmd_header *hdr = (struct hba_cmd_header *)clb;
    hdr[0].ctba = port_ctba_phys(p);
    hdr[0].ctbau = 0;

    port_start(p);
}

/* find a free command slot (we only use slot 0, single-threaded) */
static int find_slot(int p) {
    (void)p;
    return 0;
}

/* issue one ATA command via DMA. buf must be physically contiguous & mapped.
   returns 0 on success. */
static int port_cmd(int p, uint8_t cmd, uint64_t lba, uint32_t count,
                    void *buf, int write) {
    /* the bounce buffer is our DMA target/source - bound the transfer to it. */
    uint32_t bytes = count * 512;
    if (bytes > AHCI_BOUNCE_PAGES * 0x1000) return DISK_ERR_FAULT;

    /* for writes, stage the caller's data into the (physically contiguous,
       known-phys) bounce buffer before the HBA reads it. */
    if (write && buf) {
        uint8_t *s = (uint8_t *)buf;
        for (uint32_t i = 0; i < bytes; i++) bounce_virt[i] = s[i];
    }

    px_wr(p, PX_IS, (uint32_t)-1);    /* clear pending interrupts */

    int slot = find_slot(p);
    /* kernel writes the command header via its VIRTUAL mapping */
    struct hba_cmd_header *hdr = (struct hba_cmd_header *)port_clb_virt(p);
    hdr += slot;
    hdr->cfl = sizeof(struct fis_reg_h2d) / sizeof(uint32_t);
    hdr->w = write ? 1 : 0;
    /* A command that moves no data - FLUSH CACHE - has NO PRDT entry. Leaving
       prdtl at 1 would point the HBA at a region with a byte count of
       `bytes - 1` = 0xFFFFFFFF, which is not a description of nothing. */
    hdr->prdtl = count ? 1 : 0;
    hdr->prdbc = 0;

    /* command table, written via VIRT */
    struct hba_cmd_table *tbl = (struct hba_cmd_table *)port_ctba_virt(p);
    uint8_t *tb = (uint8_t *)tbl;
    for (unsigned i = 0; i < sizeof(struct hba_cmd_table); i++) tb[i] = 0;

    if (count) {
        /* one PRDT entry covering the bounce buffer; the HBA needs the
           PHYSICAL address of the data region. */
        tbl->prdt[0].dba = bounce_phys;
        tbl->prdt[0].dbau = 0;
        tbl->prdt[0].dbc = bytes - 1;     /* byte count - 1 */
        tbl->prdt[0].i = 0;
    }

    /* build the command FIS (in the command table, via VIRT) */
    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = cmd;
    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = 1 << 6;             /* LBA mode */
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->countl = (uint8_t)(count & 0xFF);
    fis->counth = (uint8_t)((count >> 8) & 0xFF);

    /* wait until the port isn't busy */
    struct timeout to;
    timer_timeout_start(&to, AHCI_TIMEOUT_MS);
    while (px_rd(p, PX_TFD) & (HBA_TFD_BSY | HBA_TFD_DRQ)) {
        if (px_rd(p, PX_TFD) & HBA_TFD_ERR) return DISK_ERR_FAULT;
        if (timer_timeout_expired(&to)) return DISK_ERR_NOT_READY;
    }

    px_wr(p, PX_CI, 1u << slot);      /* issue the command */

    /* wait for completion */
    timer_timeout_start(&to, AHCI_TIMEOUT_MS);
    for (;;) {
        if (!(px_rd(p, PX_CI) & (1u << slot))) break;
        if (px_rd(p, PX_IS) & HBA_PxIS_TFES) return DISK_ERR_FAULT;   /* task file error */
        if (timer_timeout_expired(&to)) return DISK_ERR_TIMEOUT;
    }
    if (px_rd(p, PX_IS) & HBA_PxIS_TFES) return DISK_ERR_FAULT;

    /* for reads, copy the DMA'd data out of the bounce into the caller's buf. */
    if (!write && buf) {
        uint8_t *d = (uint8_t *)buf;
        for (uint32_t i = 0; i < bytes; i++) d[i] = bounce_virt[i];
    }

    return DISK_OK;
}

/* read the IDENTIFY data for a port to get model + sector count */
static void port_identify(int p) {
    /* identify data is 512 bytes; read into our DMA region scratch (reuse
       a per-port scratch page just above the structures wouldn't fit; use a
       static buffer that we know is identity-mapped) */
    static uint8_t idbuf[512];
    if (port_cmd(p, ATA_CMD_IDENTIFY, 0, 1, idbuf, 0) != 0) {
        port_present[p] = 0;
        return;
    }
    uint16_t *id = (uint16_t *)idbuf;

    /* total addressable sectors: words 100-103 (48-bit) */
    uint64_t sectors = 0;
    sectors |= (uint64_t)id[100];
    sectors |= (uint64_t)id[101] << 16;
    sectors |= (uint64_t)id[102] << 32;
    sectors |= (uint64_t)id[103] << 48;
    if (sectors == 0) {
        /* fall back to 28-bit words 60-61 */
        sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    }
    port_sectors[p] = sectors;

    /* model string: words 27-46, byte-swapped */
    for (int i = 0; i < 20; i++) {
        uint16_t w = id[27 + i];
        port_model[p][i*2]   = (char)(w >> 8);
        port_model[p][i*2+1] = (char)(w & 0xFF);
    }
    port_model[p][40] = '\0';
    port_ssd[p] = (id[217] == 0x0001) ? 1 : 0;  /* rotation rate 1 = SSD */
    /* trim trailing spaces */
    for (int i = 39; i >= 0 && port_model[p][i] == ' '; i--) port_model[p][i] = '\0';
}

int ahci_init(void) {
    ndisks = 0;
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        port_present[i] = 0;
        port_sectors[i] = 0;
        port_model[i][0] = '\0';
    }

    /* find an AHCI controller: class 0x01, subclass 0x06, prog-if 0x01 */
    const struct pci_device *dev = pci_find(0x01, 0x06, 0x01, 0);
    if (!dev) return 0;

    /* ABAR is BAR5; mask off the low flag bits to get the physical base */
    uint32_t abar = dev->bar[5] & 0xFFFFFFF0u;
    if (abar == 0) return 0;

    /* enable bus mastering + memory space in PCI command register (offset 0x04) */
    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);   /* memory space enable | bus master enable */
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    /* map the HBA registers (ABAR region, ~8KB covers the HBA + ports). The
       ABAR is a hardware MMIO physical address (not RAM the PMM manages), so
       identity-mapping it (virt == phys) is correct, and the window opens on
       that address. */
    /* VM_UNCACHED: the ABAR is a register window. Port status, the command
       issue/completion bits and the interrupt status are all changed by the HBA
       behind the CPU's back, so caching them means polling a stale copy - which
       on real hardware shows up as a command that never appears to complete.
       The DMA structures allocated in ahci_dma_init stay cacheable; x86 keeps
       those coherent by snooping. */
    volatile uint8_t *mmio = (volatile uint8_t *)paging_map_phys(abar, 0x2000, VM_WRITE | VM_UNCACHED);
    if (!mmio || iowin_mmio(&hba, mmio, HBA_PORT(AHCI_MAX_PORTS)) != 0) return 0;

    /* allocate + map the DMA structures region and bounce buffer from the PMM
       (v5: no identity-mapped low region; DMA memory is PMM-backed and mapped
       into a kernel-virtual window, with physical addresses handed to the HBA). */
    if (ahci_dma_init() != 0) return 0;

    /* request ownership from BIOS if the handoff bit is supported (CAP2.BOH) */
    if (hba_rd(HBA_CAP2) & 0x01) {
        hba_wr(HBA_BOHC, hba_rd(HBA_BOHC) | (1 << 1));   /* OS ownership request */
        uint32_t spin = 1000000;
        while ((hba_rd(HBA_BOHC) & 0x01) && spin--) { }  /* wait for BIOS to release */
    }

    /* enable AHCI mode in global host control */
    hba_wr(HBA_GHC, hba_rd(HBA_GHC) | (1u << 31));   /* AHCI enable */

    /* probe each implemented port */
    uint32_t pi = hba_rd(HBA_PI);
    for (int p = 0; p < AHCI_MAX_PORTS; p++) {
        if (!(pi & (1u << p))) continue;

        /* device detection: SSTS DET = 3 (present + comm), IPM = 1 (active) */
        uint32_t ssts = px_rd(p, PX_SSTS);
        uint8_t det = ssts & 0x0F;
        uint8_t ipm = (ssts >> 8) & 0x0F;
        if (det != 3 || ipm != 1) continue;

        /* only handle SATA disk signature */
        if (px_rd(p, PX_SIG) != SATA_SIG_ATA) continue;

        port_rebase(p);
        port_present[p] = 1;
        port_identify(p);
        if (port_present[p]) {
            ndisks++;
            /* bridge into the unified disk registry (same step ATA needs) so
               the filesystem layer can find AHCI disks. unit = port number. */
            enum disk_media media = port_ssd[p] ? DISK_MEDIA_SSD : DISK_MEDIA_HDD;
            /* Checked for the same reason ATA checks it: DISK_MAX is 16 and a
               controller can present 32 ports, so this is reachable on real
               hardware rather than theoretical. A refused port is a disk
               nothing above can see. */
            if (disk_register(DISK_DRV_AHCI, (uint8_t)p, media, DISK_ATTACH_INTERNAL,
                              port_model[p], port_sectors[p]) < 0)
                klog_u32("AHCI", SEV_WARN, "disk registry full, port not registered: ",
                         (uint32_t)p, LOG_COLOR_VALUE, "");
        }
    }

    return ndisks;
}

int ahci_present(int port) {
    if (port < 0 || port >= AHCI_MAX_PORTS) return 0;
    return port_present[port];
}

uint64_t ahci_sectors(int port) {
    if (port < 0 || port >= AHCI_MAX_PORTS || !port_present[port]) return 0;
    return port_sectors[port];
}

int ahci_is_ssd(int port) {
    if (port < 0 || port >= AHCI_MAX_PORTS || !port_present[port]) return 0;
    return port_ssd[port];
}

const char *ahci_model(int port) {
    if (port < 0 || port >= AHCI_MAX_PORTS || !port_present[port]) return "";
    return port_model[port];
}

int ahci_read(int port, uint64_t lba, uint32_t count, void *buf) {
    if (!ahci_present(port) || count == 0) return -1;
    return port_cmd(port, ATA_CMD_READ_DMA_EX, lba, count, buf, 0);
}

int ahci_write(int port, uint64_t lba, uint32_t count, const void *buf) {
    if (!ahci_present(port) || count == 0) return -1;
    return port_cmd(port, ATA_CMD_WRITE_DMA_EX, lba, count, (void *)buf, 1);
}

/* FLUSH CACHE EXT: commit the drive's write cache to the platter.
 *
 * This backend had no flush at all, so an AHCI disk acknowledged a write as
 * soon as the data reached its cache and a power cut lost it. The ATA backend
 * flushes after every write, which meant durability silently depended on
 * whether the disk arrived through IDE or AHCI. A command that moves no data,
 * hence count 0 and no buffer. */
int ahci_flush(int port) {
    if (!ahci_present(port)) return DISK_ERR_NO_DEVICE;
    return port_cmd(port, ATA_CMD_FLUSH_EX, 0, 0, 0, 0);
}