/* /CXLite/kernel/drivers/e1000.c */
/* Aurora Tejeda */
/* Intel e1000 (82540EM) NIC driver - Stage 1: bring-up + raw frame TX/RX. */

#include "e1000.h"
#include "pci.h"
#include "paging.h"
#include "logging.h"

/* ---- e1000 register offsets (from the MMIO base, BAR0) ---- */
#define E1000_CTRL     0x0000   /* device control */
#define E1000_STATUS   0x0008   /* device status */
#define E1000_EERD     0x0014   /* EEPROM read */
#define E1000_ICR      0x00C0   /* interrupt cause read */
#define E1000_IMS      0x00D0   /* interrupt mask set */
#define E1000_IMC      0x00D8   /* interrupt mask clear */
#define E1000_RCTL     0x0100   /* receive control */
#define E1000_TCTL     0x0400   /* transmit control */
#define E1000_TIPG     0x0410   /* transmit inter-packet gap */
#define E1000_RDBAL    0x2800   /* RX descriptor base low */
#define E1000_RDBAH    0x2804   /* RX descriptor base high */
#define E1000_RDLEN    0x2808   /* RX descriptor ring length */
#define E1000_RDH      0x2810   /* RX descriptor head */
#define E1000_RDT      0x2818   /* RX descriptor tail */
#define E1000_TDBAL    0x3800   /* TX descriptor base low */
#define E1000_TDBAH    0x3804   /* TX descriptor base high */
#define E1000_TDLEN    0x3808   /* TX descriptor ring length */
#define E1000_TDH      0x3810   /* TX descriptor head */
#define E1000_TDT      0x3818   /* TX descriptor tail */
#define E1000_MTA      0x5200   /* multicast table array (128 entries) */
#define E1000_RAL      0x5400   /* receive address low (MAC) */
#define E1000_RAH      0x5404   /* receive address high (MAC) */

/* CTRL bits */
#define CTRL_SLU       (1u << 6)    /* set link up */
#define CTRL_FD        (1u << 0)    /* full duplex */

/* RCTL bits */
#define RCTL_EN        (1u << 1)    /* receiver enable */
#define RCTL_BAM       (1u << 15)   /* broadcast accept */
#define RCTL_SECRC     (1u << 26)   /* strip CRC */
#define RCTL_BSIZE_2048 0           /* buffer size 2048 (BSIZE=00, no BSEX) */

/* TCTL bits */
#define TCTL_EN        (1u << 1)    /* transmit enable */
#define TCTL_PSP       (1u << 3)    /* pad short packets */
#define TCTL_CT_SHIFT  4            /* collision threshold */
#define TCTL_COLD_SHIFT 12          /* collision distance */

/* RX descriptor status */
#define RXD_STAT_DD    (1u << 0)    /* descriptor done */
#define RXD_STAT_EOP   (1u << 1)    /* end of packet */

/* TX descriptor command + status */
#define TXD_CMD_EOP    (1u << 0)    /* end of packet */
#define TXD_CMD_IFCS   (1u << 1)    /* insert FCS */
#define TXD_CMD_RS     (1u << 3)    /* report status */
#define TXD_STAT_DD    (1u << 0)    /* descriptor done */

/* STATUS bits */
#define STATUS_LU      (1u << 1)    /* link up */

/* ring sizes (must be multiples of 8; descriptors are 16 bytes) */
#define NUM_RX_DESC    32
#define NUM_TX_DESC    32
#define RX_BUF_SIZE    2048
#define TX_BUF_SIZE    2048

/* fixed DMA region for the NIC (clear of AHCI 0x500000, OHCI 0x600000) */
/* ---- DMA region: physical vs virtual ----
 * The NIC is a bus master: descriptor and buffer addresses written into its
 * registers must be PHYSICAL. The CPU also has to touch those same structures,
 * and that access must work from ANY address space - including a ring-3
 * process's, because a shell `ping` reaches this code through SYS_NET_OP while
 * CR3 points at the shell's page directory.
 *
 * addr_space_init_pd() zeroes the whole user half for each new process, so an
 * identity mapping at 0x700000 exists only in the kernel's own directory. That
 * is why writing TX_BUF_ADDR faulted (CR2 = 0x712000) the moment ping ran from
 * the shell rather than from boot context.
 *
 * Fix: map the region into the KERNEL half, which addr_space_init_pd copies
 * into every process directory and paging's PDE hook propagates to live ones.
 * The CPU uses the 0xC07..... alias; the NIC still gets 0x007.....
 */
#define E1000_DMA_BASE 0x700000u                 /* PHYSICAL - programmed into the NIC */
#define E1000_DMA_VBASE 0xE1000000u              /* kernel-half alias - used by the CPU.
                                                   Sits beside AHCI's window (0xE0000000)
                                                   and clear of the kernel image/heap at
                                                   0xC01xxxxx. Region is 0x22000 (136 KB). */
#define DMA_V(phys) ((phys) - E1000_DMA_BASE + E1000_DMA_VBASE)
/* layout: RX descriptors, TX descriptors, then RX buffers, then TX buffers */
#define RX_DESC_ADDR   (E1000_DMA_BASE + 0x0000)
#define TX_DESC_ADDR   (E1000_DMA_BASE + 0x1000)
#define RX_BUF_ADDR    (E1000_DMA_BASE + 0x2000)              /* 32 * 2048 = 64KB */
#define TX_BUF_ADDR    (E1000_DMA_BASE + 0x12000)             /* after RX bufs */
#define E1000_DMA_END  (TX_BUF_ADDR + NUM_TX_DESC * TX_BUF_SIZE)

/* legacy RX descriptor (16 bytes) */
struct e1000_rx_desc {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint16_t checksum;
    volatile uint8_t  status;
    volatile uint8_t  errors;
    volatile uint16_t special;
} __attribute__((packed));

/* legacy TX descriptor (16 bytes) */
struct e1000_tx_desc {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint8_t  cso;
    volatile uint8_t  cmd;
    volatile uint8_t  status;
    volatile uint8_t  css;
    volatile uint16_t special;
} __attribute__((packed));

static volatile uint8_t *regs = 0;
static int present = 0;
static uint8_t mac[6];

/* diagnostics: visible from the shell via ifconfig, so a silent TX or RX
   failure shows up as a number instead of needing a packet capture */
static uint32_t stat_tx_ok, stat_tx_fail, stat_rx_ok;
static int use_eeprom = 1;

static int rx_cur = 0;
static int tx_cur = 0;

static uint32_t mmio_rd(uint32_t off)            { return *(volatile uint32_t *)(regs + off); }
static void     mmio_wr(uint32_t off, uint32_t v){ *(volatile uint32_t *)(regs + off) = v; }

static void busy_delay(uint32_t loops) {
    volatile uint32_t x = 0;
    for (uint32_t i = 0; i < loops; i++) x += i;
}

/* read a word from the EEPROM (for the MAC). returns 0xFFFF if no EEPROM. */
static uint16_t eeprom_read(uint8_t addr) {
    mmio_wr(E1000_EERD, ((uint32_t)addr << 8) | 1);   /* start read */
    uint32_t spin = 1000000;
    uint32_t v;
    while (spin--) {
        v = mmio_rd(E1000_EERD);
        if (v & (1 << 4)) break;     /* DONE bit */
    }
    if (!(v & (1 << 4))) { use_eeprom = 0; return 0xFFFF; }
    return (uint16_t)((v >> 16) & 0xFFFF);
}

/* read the MAC: try the EEPROM first, else read from the RAL/RAH registers
   (QEMU pre-loads the receive-address registers). */
static void read_mac(void) {
    uint16_t w0 = eeprom_read(0);
    if (use_eeprom && w0 != 0xFFFF) {
        uint16_t w1 = eeprom_read(1);
        uint16_t w2 = eeprom_read(2);
        mac[0] = w0 & 0xFF; mac[1] = (w0 >> 8) & 0xFF;
        mac[2] = w1 & 0xFF; mac[3] = (w1 >> 8) & 0xFF;
        mac[4] = w2 & 0xFF; mac[5] = (w2 >> 8) & 0xFF;
    } else {
        uint32_t ral = mmio_rd(E1000_RAL);
        uint32_t rah = mmio_rd(E1000_RAH);
        mac[0] = ral & 0xFF;       mac[1] = (ral >> 8) & 0xFF;
        mac[2] = (ral >> 16) & 0xFF; mac[3] = (ral >> 24) & 0xFF;
        mac[4] = rah & 0xFF;       mac[5] = (rah >> 8) & 0xFF;
    }
}

static void init_rx(void) {
    struct e1000_rx_desc *rxd = (struct e1000_rx_desc *)DMA_V(RX_DESC_ADDR);
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rxd[i].addr = (uint64_t)(RX_BUF_ADDR + (uint32_t)i * RX_BUF_SIZE);
        rxd[i].status = 0;
    }
    mmio_wr(E1000_RDBAL, RX_DESC_ADDR);
    mmio_wr(E1000_RDBAH, 0);
    mmio_wr(E1000_RDLEN, NUM_RX_DESC * 16);
    mmio_wr(E1000_RDH, 0);
    mmio_wr(E1000_RDT, NUM_RX_DESC - 1);
    rx_cur = 0;
    mmio_wr(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

static void init_tx(void) {
    struct e1000_tx_desc *txd = (struct e1000_tx_desc *)DMA_V(TX_DESC_ADDR);
    for (int i = 0; i < NUM_TX_DESC; i++) {
        txd[i].addr = (uint64_t)(TX_BUF_ADDR + (uint32_t)i * TX_BUF_SIZE);
        txd[i].status = TXD_STAT_DD;   /* mark free */
        txd[i].cmd = 0;
    }
    mmio_wr(E1000_TDBAL, TX_DESC_ADDR);
    mmio_wr(E1000_TDBAH, 0);
    mmio_wr(E1000_TDLEN, NUM_TX_DESC * 16);
    mmio_wr(E1000_TDH, 0);
    mmio_wr(E1000_TDT, 0);
    tx_cur = 0;
    mmio_wr(E1000_TCTL, TCTL_EN | TCTL_PSP
            | (15u << TCTL_CT_SHIFT) | (64u << TCTL_COLD_SHIFT));
    mmio_wr(E1000_TIPG, 0x0060200A);   /* recommended IPG */
}

int e1000_init(void) {
    present = 0;

    /* find an Ethernet NIC (class 0x02, subclass 0x00) that's Intel (8086) */
    const struct pci_device *dev = 0;
    for (unsigned i = 0; ; i++) {
        const struct pci_device *d = pci_find(0x02, 0x00, -1, i);
        if (!d) break;
        if (d->vendor_id == 0x8086) { dev = d; break; }
    }
    if (!dev) return 0;

    uint32_t base = dev->bar[0] & 0xFFFFFFF0u;
    if (base == 0) return 0;

    /* enable memory space + bus master */
    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    /* Map the register region (128KB for e1000). This is identity-mapped, which
       is only safe because PCI MMIO BARs land high - above 0xC0000000, i.e. in
       the shared kernel half, so the mapping is visible from every address
       space. If a BAR ever came back low it would be in the per-process user
       half and would fault exactly like the DMA region did. Refuse rather than
       fault mysteriously later. */
    if (base < 0xC0000000u) {
        klog_u32("E1000", SEV_WARN, "BAR0 below the kernel half: ", base, LOG_COLOR_VALUE,
                 " - would not be visible from a process address space");
        return 0;
    }
    /* PAGE_NO_CACHE: these are device registers, not memory. Reads have side
       effects and the device changes status bits under us, so a cached line
       would serve stale values. The DMA region mapped below is deliberately
       left cacheable - x86 snoops DMA, so write-back is both correct and
       faster there. */
    for (uint32_t off = 0; off < 0x20000; off += 0x1000)
        paging_map(base + off, base + off, PAGE_PRESENT | PAGE_WRITE | PAGE_NO_CACHE);
    regs = (volatile uint8_t *)base;

    /* map the NIC DMA region */
    /* map the DMA region into the KERNEL half so every address space sees it */
    for (uint32_t a = E1000_DMA_BASE; a < E1000_DMA_END; a += 0x1000)
        paging_map(DMA_V(a), a, PAGE_PRESENT | PAGE_WRITE);

    /* mask off all interrupts (we poll in stage 1) */
    mmio_wr(E1000_IMC, 0xFFFFFFFF);

    /* read the MAC address */
    read_mac();

    /* clear the multicast table */
    for (int i = 0; i < 128; i++) mmio_wr(E1000_MTA + i * 4, 0);

    /* bring the link up: set link up + full duplex (10 Mbps FDX target) */
    uint32_t ctrl = mmio_rd(E1000_CTRL);
    ctrl |= CTRL_SLU | CTRL_FD;
    mmio_wr(E1000_CTRL, ctrl);
    busy_delay(2000000);

    init_rx();
    init_tx();

    present = 1;
    return 1;
}

int e1000_present(void) { return present; }
void e1000_stats(uint32_t *tx_ok, uint32_t *tx_fail, uint32_t *rx_ok) {
    if (tx_ok)   *tx_ok   = stat_tx_ok;
    if (tx_fail) *tx_fail = stat_tx_fail;
    if (rx_ok)   *rx_ok   = stat_rx_ok;
}
const uint8_t *e1000_mac(void) { return mac; }

int e1000_link_up(void) {
    if (!present) return 0;
    return (mmio_rd(E1000_STATUS) & STATUS_LU) ? 1 : 0;
}

int e1000_send(const void *frame, uint16_t len) {
    if (!present) return -1;
    if (len > TX_BUF_SIZE) len = TX_BUF_SIZE;

    struct e1000_tx_desc *txd = (struct e1000_tx_desc *)DMA_V(TX_DESC_ADDR);
    int idx = tx_cur;

    /* The buffer has TWO addresses and they are not interchangeable:
         buf_phys - what the NIC bus-masters from (no MMU involved)
         buf      - the kernel-half alias the CPU writes through
       Handing the virtual one to the descriptor makes the NIC read nonexistent
       physical memory and silently transmit nothing. */
    uint32_t buf_phys = TX_BUF_ADDR + (uint32_t)idx * TX_BUF_SIZE;
    uint8_t *buf = (uint8_t *)DMA_V(buf_phys);
    const uint8_t *src = (const uint8_t *)frame;
    for (uint16_t i = 0; i < len; i++) buf[i] = src[i];

    txd[idx].addr = (uint64_t)buf_phys;      /* PHYSICAL - the NIC reads this */
    txd[idx].length = len;
    txd[idx].cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    txd[idx].status = 0;

    tx_cur = (tx_cur + 1) % NUM_TX_DESC;
    mmio_wr(E1000_TDT, tx_cur);

    /* wait (bounded) for the descriptor to be marked done */
    uint32_t spin = 2000000;
    while (!(txd[idx].status & TXD_STAT_DD) && spin--) busy_delay(10);
    if (txd[idx].status & TXD_STAT_DD) { stat_tx_ok++; return 0; }
    stat_tx_fail++;
    return -1;
}

int e1000_receive(void *buf, uint16_t max_len) {
    if (!present) return 0;
    struct e1000_rx_desc *rxd = (struct e1000_rx_desc *)DMA_V(RX_DESC_ADDR);
    int idx = rx_cur;

    if (!(rxd[idx].status & RXD_STAT_DD)) return 0;   /* nothing received */

    uint16_t len = rxd[idx].length;
    if (len > max_len) len = max_len;
    uint8_t *src = (uint8_t *)DMA_V(RX_BUF_ADDR + (uint32_t)idx * RX_BUF_SIZE);
    uint8_t *dst = (uint8_t *)buf;
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    /* hand the descriptor back to the NIC and advance the tail */
    rxd[idx].status = 0;
    mmio_wr(E1000_RDT, idx);
    rx_cur = (rx_cur + 1) % NUM_RX_DESC;
    stat_rx_ok++;
    return len;
}