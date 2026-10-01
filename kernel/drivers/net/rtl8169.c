/* /kernel/drivers/net/rtl8169.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Realtek RTL8111 / RTL8168 / RTL8169 driver. See rtl8169.h for why one driver
   covers all three names, and for the fact that this is unverified on silicon. */

#include "rtl8169.h"
#include "pci.h"
#include "paging.h"
#include "logging.h"

/* ---- register map (offsets from the MMIO base) --------------------------
 * Access WIDTH matters on this chip: several of these are byte or word
 * registers sharing a dword, and a 32-bit write to CR would clobber TPPoll and
 * the interrupt registers beside it. The rd/wr helpers below are width-typed
 * for that reason and the width is not interchangeable. */
#define R_IDR0        0x00   /* MAC address, 6 bytes                          */
#define R_MAR0        0x08   /* multicast hash, 8 bytes                       */
#define R_TNPDS       0x20   /* Tx Normal Priority Descriptor Start, 64-bit   */
#define R_CR          0x37   /* Command Register, 8-bit                       */
#define R_TPPOLL      0x38   /* Transmit Priority Polling, 8-bit              */
#define R_IMR         0x3C   /* Interrupt Mask, 16-bit                        */
#define R_ISR         0x3E   /* Interrupt Status, 16-bit                      */
#define R_TCR         0x40   /* Tx Configuration, 32-bit                      */
#define R_RCR         0x44   /* Rx Configuration, 32-bit                      */
#define R_CR9346      0x50   /* EEPROM / config-register lock, 8-bit          */
#define R_PHYSTATUS   0x6C   /* PHY status, 8-bit                             */
#define R_RMS         0xDA   /* Rx Max packet Size, 16-bit                    */
#define R_CPCR        0xE0   /* "C+" Command Register, 16-bit                 */
#define R_RDSAR       0xE4   /* Rx Descriptor Start Address, 64-bit           */
#define R_MTPS        0xEC   /* Max Transmit Packet Size, 8-bit               */

/* CR (0x37) */
#define CR_RST        0x10
#define CR_RE         0x08
#define CR_TE         0x04

/* TPPoll (0x38) - writing NPQ tells the NIC to re-read the Tx ring */
#define TPPOLL_NPQ    0x40

/* CR9346 (0x50) - the config registers are write-protected until unlocked */
#define CR9346_UNLOCK 0xC0
#define CR9346_LOCK   0x00

/* RCR (0x44) */
#define RCR_AAP       0x00000001   /* accept all physical (promiscuous)       */
#define RCR_APM       0x00000002   /* accept physical match (our MAC)         */
#define RCR_AM        0x00000004   /* accept multicast                        */
#define RCR_AB        0x00000008   /* accept broadcast - ARP needs this       */
#define RCR_MXDMA_UNL (7u << 8)    /* unlimited DMA burst                     */
#define RCR_RXFTH_NONE (7u << 13)  /* no Rx FIFO threshold: forward on whole frame */

/* TCR (0x40) */
#define TCR_MXDMA_UNL (7u << 8)
#define TCR_IFG_NORMAL (3u << 24)  /* standard 9.6us inter-frame gap          */

/* CPCR (0xE0) */
#define CPCR_PCIMULRW 0x0008       /* allow PCI multiple read/write           */
#define CPCR_RXCHKSUM 0x0020       /* Rx checksum offload - left OFF; the stack
                                      above validates its own checksums and an
                                      offload that silently rewrites status
                                      bits is a debugging hazard here */
#define CPCR_RXVLAN   0x0040       /* VLAN tag stripping - also left OFF      */

/* PHYSTATUS (0x6C) */
#define PHY_LINKSTS   0x02
#define PHY_FULLDUP   0x01
#define PHY_10M       0x04
#define PHY_100M      0x08
#define PHY_1000M     0x10

/* ---- descriptors --------------------------------------------------------
 * 16 bytes, and the ring base must be 256-byte aligned (the low bits of
 * TNPDS/RDSAR are not writable). The layout below puts each ring at the start
 * of its own page, which satisfies that with room to spare. */
#define DESC_OWN      0x80000000u  /* NIC owns this descriptor                */
#define DESC_EOR      0x40000000u  /* end of ring - wraps back to the base    */
#define DESC_FS       0x20000000u  /* first segment of a frame                */
#define DESC_LS       0x10000000u  /* last segment of a frame                 */
#define RX_LEN_MASK   0x00003FFFu  /* 14 bits of received length              */
#define TX_LEN_MASK   0x0000FFFFu
#define RX_RES        0x00200000u  /* receive error summary                   */

struct rtl_desc {
    volatile uint32_t opts1;
    volatile uint32_t opts2;
    volatile uint64_t addr;        /* PHYSICAL buffer address                 */
} __attribute__((packed));

#define NUM_RX_DESC   32
#define NUM_TX_DESC   32
#define RX_BUF_SIZE   2048
#define TX_BUF_SIZE   2048

/* ---- DMA window ---------------------------------------------------------
 * Same arrangement as e1000, and for the same reason: the NIC bus-masters from
 * PHYSICAL addresses, while the CPU must reach the very same structures from
 * ANY address space - including a ring-3 process's, because a shell `ping`
 * arrives here through SYS_NET_OP with CR3 pointing at the shell's directory.
 * An identity mapping would live only in the kernel's own directory and fault
 * the moment it was touched from a process. So the CPU uses a KERNEL-half
 * alias, which addr_space_init_pd copies into every new directory.
 *
 * 0x730000 sits in the window pmm.c already reserves for driver DMA
 * (0x500000-0x800000), just past e1000's region which ends at 0x722000. It
 * therefore needs no change to the PMM - but it also means the two NICs must
 * never have their windows resized into each other. */
#define RTL_DMA_BASE   0x730000u
#define RTL_DMA_VBASE  0xE2000000u   /* beside AHCI 0xE0000000 and e1000 0xE1000000 */
#define DMA_V(phys)    ((phys) - RTL_DMA_BASE + RTL_DMA_VBASE)

#define RX_DESC_ADDR   (RTL_DMA_BASE + 0x0000)
#define TX_DESC_ADDR   (RTL_DMA_BASE + 0x1000)
#define RX_BUF_ADDR    (RTL_DMA_BASE + 0x2000)                 /* 32 * 2048 = 64 KB */
#define TX_BUF_ADDR    (RX_BUF_ADDR + NUM_RX_DESC * RX_BUF_SIZE)
#define RTL_DMA_END    (TX_BUF_ADDR + NUM_TX_DESC * TX_BUF_SIZE)

static volatile uint8_t *regs = 0;
static int present = 0;
static int link = 0;
static uint8_t mac[6];
static uint32_t stat_tx_ok, stat_tx_fail, stat_rx_ok;
static int rx_cur = 0;
static int tx_cur = 0;

static struct rtl_desc *rxd = 0;
static struct rtl_desc *txd = 0;

/* ---- MMIO accessors, width-typed (see the register map note) ------------- */
static inline uint8_t  rd8(uint32_t o)  { return *(volatile uint8_t  *)(regs + o); }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(regs + o); }
static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
static inline void wr8(uint32_t o, uint8_t v)   { *(volatile uint8_t  *)(regs + o) = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(regs + o) = v; }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }

static void busy_delay(volatile uint32_t n) { while (n--) { } }

/* ---- PCI probe ----------------------------------------------------------
 * Realtek ships several device IDs across the family. 8168 is what an RTL8111
 * on a desktop board reports and is the case that matters here; the others are
 * accepted because they are the same descriptor model and refusing them would
 * be an arbitrary restriction. */
static int is_realtek_nic(uint16_t device_id) {
    switch (device_id) {
        case 0x8168:   /* RTL8111/8168 - PCIe, the common motherboard part */
        case 0x8161:   /* RTL8168 variant                                  */
        case 0x8136:   /* RTL8101/8102 fast ethernet, same rings           */
        case 0x8169:   /* RTL8169 - original PCI gigabit                   */
        case 0x8167:   /* RTL8110SC/8169SC - PCI                           */
            return 1;
        default:
            return 0;
    }
}

/* Pick the register BAR. The PCIe parts (8168/8111/8136) put MMIO at BAR2 and
 * leave BAR0 as an I/O port window; the older PCI parts (8169/8167) use BAR1.
 * Rather than branch on the device ID - the mapping is not perfectly consistent
 * across revisions - take the first usable MEMORY BAR among 2 then 1 then 0.
 *
 * pci_bar_mmio32 does the decoding, including the 64-bit BAR pairing that used
 * to be open-coded here, and reports why a BAR is unusable. */
static uint32_t pick_mmio_bar(const struct pci_device *dev) {
    static const int order[3] = { 2, 1, 0 };
    for (int i = 0; i < 3; i++) {
        uint32_t base = pci_bar_mmio32(dev, order[i], "RTL8169");
        if (base) return base;
    }
    return 0;
}

static void read_mac(void) {
    for (int i = 0; i < 6; i++) mac[i] = rd8(R_IDR0 + (uint32_t)i);
}

/* Soft reset. The chip clears CR_RST itself when it is done; the datasheet
   gives no maximum, so this is bounded rather than a `while` that could hang
   the whole boot on a wedged or absent chip. */
static int soft_reset(void) {
    wr8(R_CR, CR_RST);
    for (int i = 0; i < 1000; i++) {
        if ((rd8(R_CR) & CR_RST) == 0) return 1;
        busy_delay(10000);
    }
    return 0;
}

static void init_rings(void) {
    rxd = (struct rtl_desc *)DMA_V(RX_DESC_ADDR);
    txd = (struct rtl_desc *)DMA_V(TX_DESC_ADDR);

    for (int i = 0; i < NUM_RX_DESC; i++) {
        uint32_t phys = RX_BUF_ADDR + (uint32_t)i * RX_BUF_SIZE;
        rxd[i].addr = (uint64_t)phys;
        rxd[i].opts2 = 0;
        /* Hand every Rx descriptor to the NIC up front, with its buffer size in
           the length field. EOR on the last one is what makes it a ring - without
           it the NIC runs off the end of the array. */
        rxd[i].opts1 = DESC_OWN | (RX_BUF_SIZE & RX_LEN_MASK)
                     | (i == NUM_RX_DESC - 1 ? DESC_EOR : 0);
    }
    for (int i = 0; i < NUM_TX_DESC; i++) {
        txd[i].addr = (uint64_t)(TX_BUF_ADDR + (uint32_t)i * TX_BUF_SIZE);
        txd[i].opts2 = 0;
        /* Tx descriptors start owned by US - OWN clear - so the first send can
           claim slot 0 immediately. Only EOR is preset. */
        txd[i].opts1 = (i == NUM_TX_DESC - 1 ? DESC_EOR : 0);
    }
    rx_cur = 0;
    tx_cur = 0;
}

int rtl8169_init(void) {
    present = 0;

    const struct pci_device *dev = 0;
    for (unsigned i = 0; ; i++) {
        const struct pci_device *d = pci_find(0x02, 0x00, -1, i);   /* class 2/0 = Ethernet */
        if (!d) break;
        if (d->vendor_id == 0x10EC && is_realtek_nic(d->device_id)) { dev = d; break; }
    }
    if (!dev) return 0;      /* not an error: most machines do not have one */

    klog_u32("RTL8169", SEV_INFO, "found Realtek NIC, device ", dev->device_id,
             LOG_COLOR_VALUE, "");

    uint32_t base = pick_mmio_bar(dev);
    if (base == 0) {
        klog("RTL8169", SEV_WARN, "no usable memory BAR - not initialising");
        return 0;
    }

    /* Without bus master the rings are never read, which looks like dead silicon. */
    pci_enable_bus_master(dev);
    /* PAGE_NO_CACHE: device registers, not memory. The chip changes status bits
       under us and reads have side effects, so a cached line would serve stale
       values. 64 KB covers the whole register file on every family member. */
    for (uint32_t off = 0; off < 0x10000; off += 0x1000)
        paging_map(base + off, base + off, PAGE_PRESENT | PAGE_WRITE | PAGE_NO_CACHE);
    regs = (volatile uint8_t *)base;

    /* DMA region into the kernel half - deliberately left CACHEABLE; x86 snoops
       bus-master DMA, so write-back is both correct and faster for buffers. */
    for (uint32_t a = RTL_DMA_BASE; a < RTL_DMA_END; a += 0x1000)
        paging_map(DMA_V(a), a, PAGE_PRESENT | PAGE_WRITE);

    if (!soft_reset()) {
        klog("RTL8169", SEV_WARN, "soft reset did not complete - chip wedged or absent");
        return 0;
    }

    read_mac();
    init_rings();

    /* ---- bring it up ----------------------------------------------------
     * Order is from the datasheet and is not arbitrary: the config registers
     * are write-protected until CR9346 is unlocked, and the 8169 family wants
     * Tx/Rx enabled in CR before TCR/RCR are programmed. Writing TCR while the
     * engine is disabled is silently ignored on some revisions. */
    wr8(R_CR9346, CR9346_UNLOCK);

    /* leave checksum/VLAN offload off (see the CPCR notes above); keep whatever
       else the chip came up with rather than guessing a whole word */
    wr16(R_CPCR, (uint16_t)((rd16(R_CPCR) & ~(CPCR_RXCHKSUM | CPCR_RXVLAN)) | CPCR_PCIMULRW));

    wr32(R_RCR, RCR_APM | RCR_AM | RCR_AB | RCR_MXDMA_UNL | RCR_RXFTH_NONE);

    wr8(R_CR, CR_TE | CR_RE);

    wr32(R_TCR, TCR_MXDMA_UNL | TCR_IFG_NORMAL);

    wr16(R_RMS, RX_BUF_SIZE);
    wr8(R_MTPS, 0x3B);          /* max Tx packet, in 128-byte units (~7.5 KB) */

    /* ring bases. The high dwords are written explicitly rather than left as
       found: the chip powers up with whatever was there, and a stale high half
       sends it bus-mastering into nowhere. */
    wr32(R_TNPDS,     TX_DESC_ADDR);
    wr32(R_TNPDS + 4, 0);
    wr32(R_RDSAR,     RX_DESC_ADDR);
    wr32(R_RDSAR + 4, 0);

    wr16(R_IMR, 0x0000);        /* we poll, like e1000 - mask everything */
    wr16(R_ISR, 0xFFFF);        /* write-1-to-clear any stale status */

    wr8(R_CR9346, CR9346_LOCK);

    /* Give auto-negotiation a moment, then record the link. This is a snapshot,
       not a wait: a cable that is unplugged now may be plugged in later, and
       rtl8169_link_up() re-reads the register each time it is asked. */
    busy_delay(2000000);
    uint8_t phy = rd8(R_PHYSTATUS);
    link = (phy & PHY_LINKSTS) ? 1 : 0;

    klog_u32("RTL8169", SEV_OK, "up, PHY status ", phy, LOG_COLOR_VALUE,
             link ? " (link up)" : " (no link - check the cable)");
    /* TCR carries the hardware revision in its upper bits. Reported raw rather
       than decoded into one of ~40 marketing names: on an untested driver the
       exact number is what makes a bug report actionable. */
    klog_u32("RTL8169", SEV_INFO, "hw revision (TCR) ", rd32(R_TCR) & 0x7CF00000u,
             LOG_COLOR_VALUE, "");

    present = 1;
    return 1;
}

int rtl8169_present(void) { return present; }
const uint8_t *rtl8169_mac(void) { return mac; }

int rtl8169_link_up(void) {
    if (!present) return 0;
    link = (rd8(R_PHYSTATUS) & PHY_LINKSTS) ? 1 : 0;
    return link;
}

int rtl8169_send(const void *frame, uint16_t len) {
    if (!present || !frame) return -1;
    if (len == 0 || len > TX_BUF_SIZE) { stat_tx_fail++; return -1; }

    int idx = tx_cur;
    /* OWN still set means the NIC has not finished with this slot: the ring is
       full. Dropping is correct here - blocking would stall a syscall on a
       device that may have no link at all. */
    if (txd[idx].opts1 & DESC_OWN) { stat_tx_fail++; return -1; }

    uint32_t buf_phys = TX_BUF_ADDR + (uint32_t)idx * TX_BUF_SIZE;
    uint8_t *buf = (uint8_t *)DMA_V(buf_phys);
    const uint8_t *src = (const uint8_t *)frame;
    for (uint16_t i = 0; i < len; i++) buf[i] = src[i];

    /* Pad to the 60-byte Ethernet minimum (the NIC appends the 4-byte FCS).
       Short frames are otherwise dropped by the far end as runts. */
    uint16_t xmit = len;
    if (xmit < 60) { for (uint16_t i = xmit; i < 60; i++) buf[i] = 0; xmit = 60; }

    txd[idx].addr = (uint64_t)buf_phys;
    txd[idx].opts2 = 0;
    /* A single-buffer frame is both the first and the last segment. OWN must be
       set LAST - the moment it lands the NIC may read the descriptor, so the
       address and length have to be visible first. */
    txd[idx].opts1 = DESC_OWN | DESC_FS | DESC_LS | (xmit & TX_LEN_MASK)
                   | (idx == NUM_TX_DESC - 1 ? DESC_EOR : 0);

    wr8(R_TPPOLL, TPPOLL_NPQ);          /* kick: re-read the normal-priority ring */
    tx_cur = (idx + 1) % NUM_TX_DESC;

    /* Bounded wait for the descriptor to come back, so stat_tx_ok means
       something. Timing out is not treated as failure: the frame may well go
       out, we just stop watching. */
    for (int i = 0; i < 100000; i++) {
        if (!(txd[idx].opts1 & DESC_OWN)) { stat_tx_ok++; return 0; }
    }
    stat_tx_ok++;
    return 0;
}

int rtl8169_receive(void *buf, uint16_t max_len) {
    if (!present || !buf) return 0;

    int idx = rx_cur;
    uint32_t opts1 = rxd[idx].opts1;
    if (opts1 & DESC_OWN) return 0;     /* NIC still owns it: nothing arrived */

    int len = 0;
    if (!(opts1 & RX_RES)) {
        len = (int)(opts1 & RX_LEN_MASK);
        /* The reported length INCLUDES the 4-byte Ethernet FCS, which the stack
           above neither wants nor expects. Forgetting this is the classic
           RTL8169 bug: every frame arrives 4 bytes too long and checksums
           against trailing garbage. */
        len -= 4;
        if (len < 0) len = 0;
        if (len > (int)max_len) len = (int)max_len;

        const uint8_t *src = (const uint8_t *)DMA_V(RX_BUF_ADDR + (uint32_t)idx * RX_BUF_SIZE);
        uint8_t *dst = (uint8_t *)buf;
        for (int i = 0; i < len; i++) dst[i] = src[i];
        if (len > 0) stat_rx_ok++;
    }

    /* Hand the descriptor back regardless - a dropped bad frame still has to
       return to the ring or the NIC starves. */
    rxd[idx].opts2 = 0;
    rxd[idx].addr = (uint64_t)(RX_BUF_ADDR + (uint32_t)idx * RX_BUF_SIZE);
    rxd[idx].opts1 = DESC_OWN | (RX_BUF_SIZE & RX_LEN_MASK)
                   | (idx == NUM_RX_DESC - 1 ? DESC_EOR : 0);
    rx_cur = (idx + 1) % NUM_RX_DESC;

    return len;
}

void rtl8169_stats(uint32_t *tx_ok, uint32_t *tx_fail, uint32_t *rx_ok) {
    if (tx_ok)   *tx_ok = stat_tx_ok;
    if (tx_fail) *tx_fail = stat_tx_fail;
    if (rx_ok)   *rx_ok = stat_rx_ok;
}
