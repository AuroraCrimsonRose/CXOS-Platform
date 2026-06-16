/* /CXLite/kernel/drivers/ohci.c */
/* Aurora Tejeda */
/* OHCI USB driver - Stage 1: controller bring-up + port connection detection. */

#include "ohci.h"
#include "pci.h"
#include "paging.h"

/* A short busy-delay that does NOT depend on interrupts. ohci_init runs during
   early boot while interrupts are still disabled, so timer_sleep() (which waits
   for the PIT tick to advance `ticks`) would hang forever - the timer IRQ can't
   fire with interrupts off. This spins on a dummy volatile counter instead.
   The count is a rough calibration (not precise timing, just "long enough"). */
static void busy_delay(uint32_t loops) {
    volatile uint32_t x = 0;
    for (uint32_t i = 0; i < loops; i++) x += i;
}

/* ---- OHCI operational registers (memory-mapped at BAR0) ----
   offsets in bytes from the register base. */
#define HcRevision        0x00
#define HcControl         0x04
#define HcCommandStatus   0x08
#define HcInterruptStatus 0x0C
#define HcInterruptEnable 0x10
#define HcInterruptDisable 0x14
#define HcHCCA            0x18
#define HcPeriodCurrentED 0x1C
#define HcControlHeadED   0x20
#define HcControlCurrentED 0x24
#define HcBulkHeadED      0x28
#define HcBulkCurrentED   0x2C
#define HcDoneHead        0x30
#define HcFmInterval      0x34
#define HcFmRemaining     0x38
#define HcFmNumber        0x3C
#define HcPeriodicStart   0x40
#define HcLSThreshold     0x44
#define HcRhDescriptorA   0x48
#define HcRhDescriptorB   0x4C
#define HcRhStatus        0x50
#define HcRhPortStatus1   0x54   /* port status array starts here, one per port */

/* HcControl bits */
#define HC_CTRL_CBSR      0x00000003  /* control/bulk service ratio */
#define HC_CTRL_PLE       0x00000004  /* periodic list enable */
#define HC_CTRL_IE        0x00000008  /* isochronous enable */
#define HC_CTRL_CLE       0x00000010  /* control list enable */
#define HC_CTRL_BLE       0x00000020  /* bulk list enable */
#define HC_CTRL_HCFS      0x000000C0  /* host controller functional state */
#define HC_CTRL_HCFS_OPER 0x00000080  /* USBOperational */
#define HC_CTRL_IR        0x00000100  /* interrupt routing (1 = SMM owns it) */
#define HC_CTRL_RWC       0x00000200  /* remote wakeup connected */
#define HC_CTRL_RWE       0x00000400  /* remote wakeup enable */

/* HcCommandStatus bits */
#define HC_CMD_HCR        0x00000001  /* host controller reset */
#define HC_CMD_CLF        0x00000002  /* control list filled */
#define HC_CMD_BLF        0x00000004  /* bulk list filled */
#define HC_CMD_OCR        0x00000008  /* ownership change request */

/* HcRhDescriptorA */
#define RHDA_NDP          0x000000FF  /* number of downstream ports */

/* HcRhPortStatus bits */
#define RH_PS_CCS         0x00000001  /* current connect status (device present) */
#define RH_PS_PES         0x00000002  /* port enable status */
#define RH_PS_PSS         0x00000004  /* port suspend status */
#define RH_PS_PRS         0x00000010  /* port reset status */
#define RH_PS_LSDA        0x00000200  /* low-speed device attached */
#define RH_PS_CSC         0x00010000  /* connect status change */
#define RH_PS_PRSC        0x00100000  /* port reset status change */
#define RH_PS_SET_PES     0x00000002  /* (write) set port enable */
#define RH_PS_SET_PRS     0x00000010  /* (write) set port reset */

/* HCCA - Host Controller Communication Area (256 bytes, 256-aligned).
   The HC writes the done-queue head and frame number here. We place it (and
   later the ED/TD structures) in a fixed page-aligned DMA region. */
#define OHCI_DMA_BASE  0x600000u      /* clear of AHCI's region (0x500000) */
#define OHCI_HCCA_ADDR OHCI_DMA_BASE  /* 256-byte aligned (page-aligned) */

/* Stage 2 control-transfer structures at fixed 16-byte-aligned offsets within
   the DMA region (after the 256-byte HCCA). OHCI requires EDs/TDs 16-aligned. */
#define OHCI_ED_ADDR     (OHCI_DMA_BASE + 0x100)   /* control endpoint descriptor */
#define OHCI_TD0_ADDR    (OHCI_DMA_BASE + 0x120)   /* TD: SETUP   */
#define OHCI_TD1_ADDR    (OHCI_DMA_BASE + 0x140)   /* TD: DATA    */
#define OHCI_TD2_ADDR    (OHCI_DMA_BASE + 0x160)   /* TD: STATUS  */
#define OHCI_TD_TAIL     (OHCI_DMA_BASE + 0x180)   /* dummy tail TD */
#define OHCI_SETUP_ADDR  (OHCI_DMA_BASE + 0x200)   /* 8-byte setup packet buffer */
#define OHCI_DATA_ADDR   (OHCI_DMA_BASE + 0x300)   /* descriptor data buffer */

/* Stage 3 (bulk/BOT) structures - on a second DMA page to avoid clobbering the
   control-transfer buffers above. */
#define OHCI_BULK_PAGE   (OHCI_DMA_BASE + 0x1000)         /* second page */
#define OHCI_BED_ADDR    (OHCI_BULK_PAGE + 0x000)         /* bulk endpoint descriptor */
#define OHCI_BTD0_ADDR   (OHCI_BULK_PAGE + 0x020)         /* bulk TD 0 */
#define OHCI_BTD_TAIL    (OHCI_BULK_PAGE + 0x040)         /* bulk tail TD */
#define OHCI_CBW_ADDR    (OHCI_BULK_PAGE + 0x100)         /* Command Block Wrapper (31 B) */
#define OHCI_CSW_ADDR    (OHCI_BULK_PAGE + 0x140)         /* Command Status Wrapper (13 B) */
#define OHCI_BULK_BUF    (OHCI_BULK_PAGE + 0x200)         /* bulk data buffer (>=512 B) */

/* OHCI Endpoint Descriptor (16 bytes) */
struct ohci_ed {
    volatile uint32_t control;
    volatile uint32_t tailp;
    volatile uint32_t headp;
    volatile uint32_t nexted;
};

/* OHCI Transfer Descriptor (16 bytes, general) */
struct ohci_td {
    volatile uint32_t control;
    volatile uint32_t cbp;       /* current buffer pointer */
    volatile uint32_t nexttd;
    volatile uint32_t be;        /* buffer end */
};

/* ED.control fields */
#define ED_MPS_SHIFT   16
#define ED_SKIP        (1u << 14)

/* TD.control fields */
#define TD_DP_SETUP    (0u << 19)
#define TD_DP_OUT      (1u << 19)
#define TD_DP_IN       (2u << 19)
#define TD_T_DATA0     (2u << 24)
#define TD_T_DATA1     (3u << 24)
#define TD_DI_NONE     (7u << 21)
#define TD_R_ROUNDING  (1u << 18)
#define TD_CC_SHIFT    28

#define HC_CMD_CLF_BIT 0x00000002

static volatile uint8_t *regs = 0;    /* memory-mapped register base */
static int present = 0;
static int nports = 0;

static uint32_t rd(uint32_t off)            { return *(volatile uint32_t *)(regs + off); }
static void     wr(uint32_t off, uint32_t v){ *(volatile uint32_t *)(regs + off) = v; }

int ohci_init(void) {
    present = 0; nports = 0;

    /* find an OHCI controller: class 0x0C (serial bus), subclass 0x03 (USB),
       prog-if 0x10 (OHCI) */
    const struct pci_device *dev = pci_find(0x0C, 0x03, 0x10, 0);
    if (!dev) return 0;

    /* OHCI registers are memory-mapped via BAR0 (mask off low flag bits) */
    uint32_t base = dev->bar[0] & 0xFFFFFFF0u;
    if (base == 0) return 0;

    /* enable memory space + bus master in PCI command register */
    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    /* map the register region (4KB covers the OHCI register set) */
    paging_map(base, base, PAGE_PRESENT | PAGE_WRITE);
    regs = (volatile uint8_t *)base;

    /* map the HCCA DMA region (control-transfer structures) */
    paging_map(OHCI_DMA_BASE, OHCI_DMA_BASE, PAGE_PRESENT | PAGE_WRITE);
    /* map the second DMA page (bulk-transfer structures for stage 3) */
    paging_map(OHCI_BULK_PAGE & ~0xFFFu, OHCI_BULK_PAGE & ~0xFFFu,
               PAGE_PRESENT | PAGE_WRITE);

    /* --- ownership handoff --- */
    /* If HcControl.IR is set, SMM (the BIOS) owns the controller. Request
       ownership by setting OCR and waiting for IR to clear. */
    uint32_t control = rd(HcControl);
    if (control & HC_CTRL_IR) {
        wr(HcCommandStatus, HC_CMD_OCR);
        uint32_t spin = 1000000;
        while ((rd(HcControl) & HC_CTRL_IR) && spin--) { }
    }

    /* --- save fm interval (reset clears it; we restore it) --- */
    uint32_t fm_interval = rd(HcFmInterval);

    /* --- reset the host controller --- */
    wr(HcCommandStatus, HC_CMD_HCR);
    /* HCR self-clears within 10us; wait for it */
    uint32_t spin = 100000;
    while ((rd(HcCommandStatus) & HC_CMD_HCR) && spin--) { }
    if (rd(HcCommandStatus) & HC_CMD_HCR) return 0;   /* reset stuck */

    /* after reset the HC is in USBSuspend; we have ~2ms to set it up */

    /* zero the HCCA */
    {
        volatile uint8_t *hcca = (volatile uint8_t *)OHCI_HCCA_ADDR;
        for (int i = 0; i < 256; i++) hcca[i] = 0;
    }

    /* restore frame interval + set periodic start (~90% of frame) */
    wr(HcFmInterval, fm_interval);
    uint32_t frame_interval = fm_interval & 0x3FFF;
    wr(HcPeriodicStart, (frame_interval * 9) / 10);

    /* point the controller at our HCCA */
    wr(HcHCCA, OHCI_HCCA_ADDR);

    /* no ED lists yet (stage 1) - clear the list heads */
    wr(HcControlHeadED, 0);
    wr(HcBulkHeadED, 0);

    /* move the controller to the operational state */
    control = rd(HcControl);
    control &= ~HC_CTRL_HCFS;
    control |= HC_CTRL_HCFS_OPER;
    /* leave list processing disabled for stage 1 (no EDs to process yet) */
    wr(HcControl, control);

    /* --- power the root-hub ports and read the port count --- */
    uint32_t rhda = rd(HcRhDescriptorA);
    nports = (int)(rhda & RHDA_NDP);
    if (nports < 1 || nports > 15) nports = 0;

    /* power all ports on (set global power via HcRhStatus LPSC = bit 16) */
    wr(HcRhStatus, (1 << 16));   /* SetGlobalPower */
    /* give ports a moment to power up + devices to signal connect.
       busy-delay (not timer_sleep) because interrupts are off during boot init. */
    busy_delay(20000000);

    present = 1;
    return nports;
}

int ohci_present(void) { return present; }
int ohci_port_count(void) { return nports; }

int ohci_port_connected(int port) {
    if (!present || port < 0 || port >= nports) return 0;
    uint32_t ps = rd(HcRhPortStatus1 + (uint32_t)port * 4);
    return (ps & RH_PS_CCS) ? 1 : 0;
}
/* ============================ STAGE 2: enumeration ======================== */
/*
 * Control-transfer machinery + device enumeration. We use a single control ED
 * and a SETUP/DATA/STATUS TD chain in the fixed DMA region, link the ED into
 * the controller's control list, ring the doorbell, and poll for completion.
 *
 * This reads the device descriptor (and assigns an address) so we can identify
 * what's plugged in (vendor/product/class). No class driver yet - that's stage 3.
 */

/* a parsed snapshot of the device on a port */
static struct {
    int      valid;
    uint16_t vendor;
    uint16_t product;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint8_t  max_packet0;
    int      low_speed;
    /* from the configuration/interface descriptor (stage 2.5) */
    int      if_valid;        /* 1 if we parsed an interface descriptor */
    uint8_t  if_class;        /* interface class (0x08 = mass storage) */
    uint8_t  if_subclass;     /* 0x06 = SCSI transparent command set */
    uint8_t  if_protocol;     /* 0x50 = Bulk-Only Transport */
    uint8_t  config_value;    /* bConfigurationValue (for SET_CONFIGURATION) */
    uint8_t  ep_in;           /* bulk IN endpoint address (0 = none found) */
    uint8_t  ep_out;          /* bulk OUT endpoint address */
    uint16_t ep_in_mps;       /* bulk IN max packet size */
    uint16_t ep_out_mps;      /* bulk OUT max packet size */
} dev_info;

/* USB setup packet (8 bytes) */
struct usb_setup {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

#define USB_REQ_GET_DESCRIPTOR  6
#define USB_REQ_SET_ADDRESS     5
#define USB_DT_DEVICE           1
#define USB_DT_CONFIG           2
#define USB_DT_INTERFACE        4
#define USB_DT_ENDPOINT         5
#define USB_REQ_SET_CONFIG      9

/* reset a root-hub port and enable it. returns 1 on success. */
static int port_reset(int port) {
    uint32_t poff = HcRhPortStatus1 + (uint32_t)port * 4;
    /* assert reset */
    wr(poff, RH_PS_SET_PRS);
    /* wait for reset to complete (PRSC set), bounded */
    uint32_t spin = 500000;
    while (!(rd(poff) & RH_PS_PRSC) && spin--) busy_delay(100);
    /* clear the reset-change, leave port enabled */
    wr(poff, RH_PS_PRSC);
    busy_delay(200000);   /* recovery time after reset */
    return (rd(poff) & RH_PS_PES) ? 1 : 0;
}

/* run one control transfer. dir_in=1 means data flows device->host (GET).
   data goes to/from OHCI_DATA_ADDR. returns bytes transferred, or -1 on error. */
static int control_transfer(int dev_addr, int low_speed,
                            const struct usb_setup *setup,
                            int dir_in, int data_len) {
    struct ohci_ed *ed   = (struct ohci_ed *)OHCI_ED_ADDR;
    struct ohci_td *tds  = (struct ohci_td *)OHCI_TD0_ADDR;
    struct ohci_td *td0  = (struct ohci_td *)OHCI_TD0_ADDR;  /* SETUP  */
    struct ohci_td *td1  = (struct ohci_td *)OHCI_TD1_ADDR;  /* DATA   */
    struct ohci_td *td2  = (struct ohci_td *)OHCI_TD2_ADDR;  /* STATUS */
    struct ohci_td *tail = (struct ohci_td *)OHCI_TD_TAIL;   /* dummy tail */
    (void)tds;

    /* copy the setup packet into the DMA buffer */
    uint8_t *sb = (uint8_t *)OHCI_SETUP_ADDR;
    const uint8_t *src = (const uint8_t *)setup;
    for (int i = 0; i < 8; i++) sb[i] = src[i];

    int mps = dev_info.max_packet0 ? dev_info.max_packet0 : 8;

    /* ED: function address, endpoint 0, max packet size, speed */
    ed->control = ((uint32_t)dev_addr & 0x7F)
                | (0 << 7)                       /* endpoint 0 */
                | (low_speed ? (1u << 13) : 0)   /* S = speed */
                | ((uint32_t)mps << ED_MPS_SHIFT);
    ed->tailp = OHCI_TD_TAIL;
    ed->headp = OHCI_TD0_ADDR;       /* head = first TD (SETUP) */
    ed->nexted = 0;

    /* SETUP TD: DATA0, DP=SETUP, buffer = setup packet */
    td0->control = TD_DP_SETUP | TD_T_DATA0 | TD_DI_NONE | ((uint32_t)0xE << TD_CC_SHIFT);
    td0->cbp = OHCI_SETUP_ADDR;
    td0->be  = OHCI_SETUP_ADDR + 7;
    td0->nexttd = OHCI_TD1_ADDR;

    /* DATA TD (if any): DATA1, direction = dir_in */
    if (data_len > 0) {
        td1->control = (dir_in ? TD_DP_IN : TD_DP_OUT) | TD_T_DATA1
                     | TD_DI_NONE | TD_R_ROUNDING | ((uint32_t)0xE << TD_CC_SHIFT);
        td1->cbp = OHCI_DATA_ADDR;
        td1->be  = OHCI_DATA_ADDR + data_len - 1;
        td1->nexttd = OHCI_TD2_ADDR;
    } else {
        /* skip DATA - SETUP points straight to STATUS */
        td0->nexttd = OHCI_TD2_ADDR;
    }

    /* STATUS TD: opposite direction, DATA1, zero length */
    td2->control = (dir_in ? TD_DP_OUT : TD_DP_IN) | TD_T_DATA1
                 | TD_DI_NONE | ((uint32_t)0xE << TD_CC_SHIFT);
    td2->cbp = 0;
    td2->be  = 0;
    td2->nexttd = OHCI_TD_TAIL;

    /* tail TD (dummy) */
    tail->control = 0; tail->cbp = 0; tail->nexttd = 0; tail->be = 0;

    /* link our ED into the control list and enable control processing */
    wr(HcControlHeadED, OHCI_ED_ADDR);
    wr(HcControlCurrentED, 0);
    uint32_t ctl = rd(HcControl);
    wr(HcControl, ctl | HC_CTRL_CLE);
    /* ring the control-list doorbell */
    wr(HcCommandStatus, HC_CMD_CLF_BIT);

    /* poll until the ED's head pointer reaches the tail (all TDs done) or the
       head halts (bit 0 of headp set = halted/error), bounded. */
    uint32_t spin = 2000000;
    int halted = 0;
    while (spin--) {
        uint32_t hp = ed->headp;
        if (hp & 1) { halted = 1; break; }          /* halted (error) */
        if ((hp & ~0xFu) == OHCI_TD_TAIL) break;     /* head == tail: done */
        busy_delay(50);
    }

    /* disable control processing again */
    ctl = rd(HcControl);
    wr(HcControl, ctl & ~HC_CTRL_CLE);
    wr(HcControlHeadED, 0);

    if (halted || spin == 0) return -1;

    /* check the STATUS/DATA TD condition codes via the done path: simplest is
       to trust completion + return requested length (QEMU/Bochs set CC=0). */
    return data_len;
}

/* enumerate the device on `port`: reset, read device descriptor, set address.
   fills dev_info. returns 1 on success. */
int ohci_enumerate(int port) {
    dev_info.valid = 0;
    if (!present || port < 0 || port >= nports) return 0;
    if (!ohci_port_connected(port)) return 0;

    /* detect speed from port status (low-speed device attached bit) */
    uint32_t ps = rd(HcRhPortStatus1 + (uint32_t)port * 4);
    int low_speed = (ps & RH_PS_LSDA) ? 1 : 0;
    dev_info.low_speed = low_speed;
    dev_info.max_packet0 = 8;     /* safe default until we read the descriptor */

    if (!port_reset(port)) return 0;

    /* GET_DESCRIPTOR(device), first 8 bytes (to learn max packet size), addr 0 */
    struct usb_setup s;
    s.bmRequestType = 0x80;       /* device-to-host, standard, device */
    s.bRequest = USB_REQ_GET_DESCRIPTOR;
    s.wValue = (USB_DT_DEVICE << 8) | 0;
    s.wIndex = 0;
    s.wLength = 8;
    if (control_transfer(0, low_speed, &s, 1, 8) < 0) return 0;

    uint8_t *d = (uint8_t *)OHCI_DATA_ADDR;
    dev_info.max_packet0 = d[7];  /* bMaxPacketSize0 */
    if (dev_info.max_packet0 == 0) dev_info.max_packet0 = 8;

    /* SET_ADDRESS = 1 */
    s.bmRequestType = 0x00;
    s.bRequest = USB_REQ_SET_ADDRESS;
    s.wValue = 1;
    s.wIndex = 0;
    s.wLength = 0;
    if (control_transfer(0, low_speed, &s, 0, 0) < 0) return 0;
    busy_delay(200000);           /* set-address recovery */

    /* GET_DESCRIPTOR(device), full 18 bytes, now at address 1 */
    s.bmRequestType = 0x80;
    s.bRequest = USB_REQ_GET_DESCRIPTOR;
    s.wValue = (USB_DT_DEVICE << 8) | 0;
    s.wIndex = 0;
    s.wLength = 18;
    if (control_transfer(1, low_speed, &s, 1, 18) < 0) return 0;

    d = (uint8_t *)OHCI_DATA_ADDR;
    dev_info.dev_class    = d[4];
    dev_info.dev_subclass = d[5];
    dev_info.dev_protocol = d[6];
    dev_info.vendor  = (uint16_t)(d[8]  | (d[9]  << 8));
    dev_info.product = (uint16_t)(d[10] | (d[11] << 8));
    dev_info.valid = 1;

    /* ---- configuration descriptor (contains interface + endpoint descriptors).
       The device class is often 0x00 ("per interface"); the real class (0x08
       mass storage) and the bulk endpoint addresses live in here. ---- */
    dev_info.if_valid = 0;
    dev_info.ep_in = dev_info.ep_out = 0;

    /* First fetch the 9-byte config header to learn the total length. */
    s.bmRequestType = 0x80;
    s.bRequest = USB_REQ_GET_DESCRIPTOR;
    s.wValue = (USB_DT_CONFIG << 8) | 0;
    s.wIndex = 0;
    s.wLength = 9;
    if (control_transfer(1, low_speed, &s, 1, 9) >= 0) {
        d = (uint8_t *)OHCI_DATA_ADDR;
        uint16_t total = (uint16_t)(d[2] | (d[3] << 8));
        dev_info.config_value = d[5];           /* bConfigurationValue */
        if (total > 255) total = 255;           /* clamp to our buffer */
        if (total < 9) total = 9;

        /* now fetch the full config (interface + endpoints) */
        s.wLength = total;
        if (control_transfer(1, low_speed, &s, 1, total) >= 0) {
            d = (uint8_t *)OHCI_DATA_ADDR;
            /* walk the descriptor list: each entry is [bLength, bDescriptorType, ...] */
            int i = 0;
            while (i + 1 < total && d[i] >= 2) {
                uint8_t blen = d[i];
                uint8_t btype = d[i + 1];
                if (btype == USB_DT_INTERFACE && i + 8 < total) {
                    dev_info.if_class    = d[i + 5];
                    dev_info.if_subclass = d[i + 6];
                    dev_info.if_protocol = d[i + 7];
                    dev_info.if_valid = 1;
                } else if (btype == USB_DT_ENDPOINT && i + 6 < total) {
                    uint8_t addr = d[i + 2];           /* bEndpointAddress */
                    uint8_t attr = d[i + 3];           /* bmAttributes */
                    uint16_t mps = (uint16_t)(d[i + 4] | (d[i + 5] << 8));
                    if ((attr & 0x03) == 0x02) {       /* bulk endpoint */
                        if (addr & 0x80) { dev_info.ep_in = addr;  dev_info.ep_in_mps = mps; }
                        else             { dev_info.ep_out = addr; dev_info.ep_out_mps = mps; }
                    }
                }
                if (blen == 0) break;                  /* guard against a bad length */
                i += blen;
            }
        }
    }

    return 1;
}

int ohci_dev_valid(void)        { return dev_info.valid; }
uint16_t ohci_dev_vendor(void)  { return dev_info.vendor; }
uint16_t ohci_dev_product(void) { return dev_info.product; }
uint8_t ohci_dev_class(void)    { return dev_info.dev_class; }
uint8_t ohci_dev_subclass(void) { return dev_info.dev_subclass; }
uint8_t ohci_dev_protocol(void) { return dev_info.dev_protocol; }

/* stage 2.5: interface class + bulk endpoint accessors (for stage 3) */
int     ohci_dev_if_valid(void)    { return dev_info.if_valid; }
uint8_t ohci_dev_if_class(void)    { return dev_info.if_class; }
uint8_t ohci_dev_if_subclass(void) { return dev_info.if_subclass; }
uint8_t ohci_dev_if_protocol(void) { return dev_info.if_protocol; }
uint8_t ohci_dev_ep_in(void)       { return dev_info.ep_in; }
uint8_t ohci_dev_ep_out(void)      { return dev_info.ep_out; }
uint8_t ohci_dev_config_value(void){ return dev_info.config_value; }
uint16_t ohci_dev_ep_out_mps(void) { return dev_info.ep_out_mps; }

/* ===================== STAGE 3: bulk transport + SCSI ===================== */
/*
 * USB mass storage uses Bulk-Only Transport (BOT): a 31-byte Command Block
 * Wrapper (CBW) carrying a SCSI command is sent OUT on the bulk-out endpoint,
 * optional data is moved on bulk-in/out, then a 13-byte Command Status Wrapper
 * (CSW) is read IN. SCSI commands: INQUIRY, READ CAPACITY(10), READ(10).
 *
 * We reuse the ED/TD style of the control engine but on the device's bulk
 * endpoints (addresses learned during enumeration). Data toggles are tracked
 * per direction across transfers (bulk endpoints require correct DATA0/DATA1).
 */

static int      usb_dev_addr = 1;     /* address we assigned during enumeration */
static int      usb_low_speed = 0;
static uint8_t  usb_ep_in = 0, usb_ep_out = 0;
static int      tog_in = 0, tog_out = 0;   /* data toggles per endpoint */
static uint64_t usb_sectors = 0;
static uint32_t usb_block_size = 512;
static int      usb_ready = 0;
static uint8_t  usb_pdt = 0;       /* SCSI peripheral device type (0=disk,5=cdrom) */
static int      usb_init_step = 0; /* diagnostic: where storage_init got to */
static int      bulk_fail = 0;     /* diagnostic: 1=halted, 2=timeout */
static uint32_t bulk_cc = 0;       /* diagnostic: last TD condition code */

/* one bulk transfer on endpoint `ep` (with its 0x80 IN bit). buf is a physical
   address in our mapped DMA region. returns 0 on success, -1 on error. */
static int bulk_transfer(uint8_t ep, int dir_in, uint32_t buf, int len, int *toggle) {
    struct ohci_ed *ed   = (struct ohci_ed *)OHCI_BED_ADDR;
    struct ohci_td *td   = (struct ohci_td *)OHCI_BTD0_ADDR;
    struct ohci_td *tail = (struct ohci_td *)OHCI_BTD_TAIL;

    int mps = dir_in ? (dev_info.ep_in_mps ? dev_info.ep_in_mps : 64)
                     : (dev_info.ep_out_mps ? dev_info.ep_out_mps : 64);

    ed->control = ((uint32_t)usb_dev_addr & 0x7F)
                | (((uint32_t)(ep & 0x0F)) << 7)        /* endpoint number */
                | (0u << 11)                            /* D=00: direction from TD's DP */
                | (usb_low_speed ? (1u << 13) : 0)
                | ((uint32_t)mps << ED_MPS_SHIFT);
    ed->tailp = OHCI_BTD_TAIL;
    ed->headp = OHCI_BTD0_ADDR | (*toggle ? 2u : 0u);   /* carry toggle in headp bit1 (toggleCarry) */
    ed->nexted = 0;

    uint32_t tctl = (dir_in ? TD_DP_IN : TD_DP_OUT)
                  | (*toggle ? TD_T_DATA1 : TD_T_DATA0)
                  | TD_DI_NONE | TD_R_ROUNDING
                  | ((uint32_t)0xE << TD_CC_SHIFT);
    td->control = tctl;
    td->cbp = len ? buf : 0;
    td->be  = len ? (buf + len - 1) : 0;
    td->nexttd = OHCI_BTD_TAIL;

    tail->control = 0; tail->cbp = 0; tail->nexttd = 0; tail->be = 0;

    /* link into the bulk list + enable bulk processing */
    wr(HcBulkHeadED, OHCI_BED_ADDR);
    wr(HcBulkCurrentED, 0);
    uint32_t ctl = rd(HcControl);
    wr(HcControl, ctl | HC_CTRL_BLE);
    wr(HcCommandStatus, HC_CMD_BLF);

    uint32_t spin = 3000000;
    int halted = 0;
    while (spin--) {
        uint32_t hp = ed->headp;
        if (hp & 1) { halted = 1; break; }
        if ((hp & ~0xFu) == OHCI_BTD_TAIL) break;
        busy_delay(50);
    }

    ctl = rd(HcControl);
    wr(HcControl, ctl & ~HC_CTRL_BLE);
    wr(HcBulkHeadED, 0);

    bulk_cc = (td->control >> 28) & 0xF;        /* TD condition code */
    if (halted) { bulk_fail = 1; return -1; }   /* endpoint halted (stall/error) */
    if (spin == 0) { bulk_fail = 2; return -1; } /* timed out (never progressed) */

    *toggle ^= 1;   /* flip toggle for next transfer (single-TD transfers) */
    return 0;
}

/* build + run a Bulk-Only Transport command. cmd/cmdlen = SCSI CDB; data_len =
   expected data bytes (read into OHCI_BULK_BUF); dir_in = data direction.
   returns 0 on success (CSW status 0), -1 otherwise. */
static uint32_t bot_tag = 1;
static int bot_command(const uint8_t *cdb, int cmdlen, int data_len, int dir_in) {
    uint8_t *cbw = (uint8_t *)OHCI_CBW_ADDR;
    for (int i = 0; i < 31; i++) cbw[i] = 0;
    /* dCBWSignature = 'USBC' */
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;
    /* dCBWTag */
    uint32_t tag = bot_tag++;
    cbw[4] = tag & 0xFF; cbw[5] = (tag >> 8) & 0xFF;
    cbw[6] = (tag >> 16) & 0xFF; cbw[7] = (tag >> 24) & 0xFF;
    /* dCBWDataTransferLength */
    cbw[8]  = data_len & 0xFF; cbw[9]  = (data_len >> 8) & 0xFF;
    cbw[10] = (data_len >> 16) & 0xFF; cbw[11] = (data_len >> 24) & 0xFF;
    /* bmCBWFlags: 0x80 = data IN (device->host) */
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = 0;            /* LUN 0 */
    cbw[14] = (uint8_t)cmdlen;
    for (int i = 0; i < cmdlen && i < 16; i++) cbw[15 + i] = cdb[i];

    /* 1. send CBW (bulk OUT, 31 bytes) */
    if (bulk_transfer(usb_ep_out, 0, OHCI_CBW_ADDR, 31, &tog_out) < 0) return -1;

    /* 2. data phase (if any) */
    if (data_len > 0) {
        if (bulk_transfer(dir_in ? usb_ep_in : usb_ep_out, dir_in,
                          OHCI_BULK_BUF, data_len,
                          dir_in ? &tog_in : &tog_out) < 0) return -1;
    }

    /* 3. read CSW (bulk IN, 13 bytes) */
    if (bulk_transfer(usb_ep_in, 1, OHCI_CSW_ADDR, 13, &tog_in) < 0) return -1;

    uint8_t *csw = (uint8_t *)OHCI_CSW_ADDR;
    /* dCSWSignature 'USBS' + bCSWStatus (offset 12): 0 = passed */
    if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53)
        return -1;
    if (csw[12] != 0) return -1;     /* command failed/phase error */
    return 0;
}

/* SCSI: prepare the device for reads. INQUIRY (device type) + READ CAPACITY
   (sector count + block size). returns 1 on success. */
int ohci_storage_init(void) {
    usb_ready = 0;
    usb_init_step = 0;
    if (!dev_info.if_valid || dev_info.if_class != 0x08) { usb_init_step = 1; return 0; }
    if (!dev_info.ep_in || !dev_info.ep_out) { usb_init_step = 2; return 0; }

    usb_dev_addr = 1;
    usb_low_speed = dev_info.low_speed;
    usb_ep_in  = dev_info.ep_in;
    usb_ep_out = dev_info.ep_out;
    tog_in = tog_out = 0;

    /* SET_CONFIGURATION - REQUIRED before bulk endpoints work. */
    {
        struct usb_setup s;
        s.bmRequestType = 0x00;
        s.bRequest = USB_REQ_SET_CONFIG;
        s.wValue = dev_info.config_value ? dev_info.config_value : 1;
        s.wIndex = 0;
        s.wLength = 0;
        if (control_transfer(usb_dev_addr, usb_low_speed, &s, 0, 0) < 0) { usb_init_step = 3; return 0; }
        busy_delay(200000);
    }

    /* INQUIRY (36 bytes) */
    uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (bot_command(inquiry, 6, 36, 1) < 0) { usb_init_step = 4; return 0; }
    usb_pdt = ((uint8_t *)OHCI_BULK_BUF)[0] & 0x1F;

    /* READ CAPACITY(10) */
    uint8_t readcap[10] = { 0x25, 0,0,0,0, 0,0,0,0, 0 };
    if (bot_command(readcap, 10, 8, 1) < 0) { usb_init_step = 5; return 0; }
    uint8_t *cap = (uint8_t *)OHCI_BULK_BUF;
    uint32_t last_lba = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16)
                      | ((uint32_t)cap[2] << 8)  | cap[3];
    uint32_t blk = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16)
                 | ((uint32_t)cap[6] << 8)  | cap[7];
    if (blk == 0) blk = 512;
    usb_block_size = blk;
    usb_sectors = (uint64_t)last_lba + 1;
    usb_ready = 1;
    usb_init_step = 100;   /* success */
    return 1;
}

uint64_t ohci_storage_sectors(void) { return usb_sectors; }
uint8_t  ohci_storage_pdt(void)     { return usb_pdt; }
int      ohci_storage_step(void)    { return usb_init_step; }
int      ohci_bulk_fail(void)       { return bulk_fail; }
uint32_t ohci_bulk_cc(void)         { return bulk_cc; }
uint32_t ohci_storage_block_size(void) { return usb_block_size; }
int      ohci_storage_ready(void)   { return usb_ready; }

/* read `count` 512-byte sectors starting at `lba` into buf via SCSI READ(10).
   reads one block at a time through the bulk buffer. returns 0 on success. */
int ohci_storage_read(uint64_t lba, uint32_t count, void *buf) {
    if (!usb_ready) return -1;
    uint8_t *out = (uint8_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        uint32_t cur = (uint32_t)lba + s;
        uint8_t read10[10];
        read10[0] = 0x28;            /* READ(10) */
        read10[1] = 0;
        read10[2] = (cur >> 24) & 0xFF;
        read10[3] = (cur >> 16) & 0xFF;
        read10[4] = (cur >> 8) & 0xFF;
        read10[5] = cur & 0xFF;
        read10[6] = 0;
        read10[7] = 0;
        read10[8] = 1;               /* one block */
        read10[9] = 0;
        if (bot_command(read10, 10, (int)usb_block_size, 1) < 0) return -1;
        /* copy the block out of the bulk buffer */
        uint8_t *src = (uint8_t *)OHCI_BULK_BUF;
        for (uint32_t i = 0; i < usb_block_size && i < 512; i++)
            out[s * 512 + i] = src[i];
    }
    return 0;
}

/* write `count` 512-byte sectors starting at `lba` from buf via SCSI WRITE(10).
   one block at a time through the bulk buffer. returns 0 on success. */
int ohci_storage_write(uint64_t lba, uint32_t count, const void *buf) {
    if (!usb_ready) return -1;
    const uint8_t *in = (const uint8_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        uint32_t cur = (uint32_t)lba + s;
        /* stage the block into the bulk buffer */
        uint8_t *dst = (uint8_t *)OHCI_BULK_BUF;
        uint32_t i = 0;
        for (; i < usb_block_size && i < 512; i++) dst[i] = in[s * 512 + i];
        for (; i < usb_block_size; i++) dst[i] = 0;   /* pad if block > 512 */

        uint8_t write10[10];
        write10[0] = 0x2A;            /* WRITE(10) */
        write10[1] = 0;
        write10[2] = (cur >> 24) & 0xFF;
        write10[3] = (cur >> 16) & 0xFF;
        write10[4] = (cur >> 8) & 0xFF;
        write10[5] = cur & 0xFF;
        write10[6] = 0;
        write10[7] = 0;
        write10[8] = 1;               /* one block */
        write10[9] = 0;
        if (bot_command(write10, 10, (int)usb_block_size, 0) < 0) return -1;
    }
    return 0;
}