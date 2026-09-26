/* /CXK/kernel/drivers/usb/ohci.c */
/* Aurora Tejeda / CATX Systems LLC */
/* OHCI host controller. See ohci.h for why this is the one the AM3+ board
   needs for its keyboard and mouse. */

#include "ohci.h"
#include "usb.h"
#include "pci.h"
#include "paging.h"
#include "logging.h"
#include "timer.h"

/* ---- operational registers (at BAR0) ------------------------------------ */
#define OHCI_REVISION        0x00
#define OHCI_CONTROL         0x04
#define OHCI_CMDSTATUS       0x08
#define OHCI_INTSTATUS       0x0C
#define OHCI_INTENABLE       0x10
#define OHCI_INTDISABLE      0x14
#define OHCI_HCCA            0x18
#define OHCI_CTRLHEADED      0x20   /* control endpoint list head             */
#define OHCI_CTRLCURRENTED   0x24
#define OHCI_BULKHEADED      0x28
#define OHCI_BULKCURRENTED   0x2C
#define OHCI_FMINTERVAL      0x34
#define OHCI_PERIODICSTART   0x40
#define OHCI_RHDESCRIPTORA   0x48
#define OHCI_RHDESCRIPTORB   0x4C
#define OHCI_RHSTATUS        0x50
#define OHCI_RHPORTSTATUS    0x54   /* array, one dword per port              */

/* HcControl */
#define CTRL_CBSR_MASK       0x00000003
#define CTRL_PLE             (1u << 2)   /* periodic list enable              */
#define CTRL_IE              (1u << 3)   /* isochronous enable                */
#define CTRL_CLE             (1u << 4)   /* control list enable               */
#define CTRL_BLE             (1u << 5)   /* bulk list enable                  */
#define CTRL_HCFS_MASK       (3u << 6)   /* host controller functional state  */
#define CTRL_HCFS_RESET      (0u << 6)
#define CTRL_HCFS_RESUME     (1u << 6)
#define CTRL_HCFS_OPERATIONAL (2u << 6)
#define CTRL_HCFS_SUSPEND    (3u << 6)
#define CTRL_IR              (1u << 8)   /* interrupt routing: 1 = SMM owns it */
#define CTRL_RWC             (1u << 9)
#define CTRL_RWE             (1u << 10)

/* HcCommandStatus */
#define CMD_HCR              (1u << 0)   /* host controller reset             */
#define CMD_CLF              (1u << 1)   /* control list filled               */
#define CMD_BLF              (1u << 2)   /* bulk list filled                  */
#define CMD_OCR              (1u << 3)   /* ownership change request          */

/* HcInterruptStatus */
#define INT_WDH              (1u << 1)   /* writeback done head               */
#define INT_RHSC             (1u << 6)   /* root hub status change            */
#define INT_OC               (1u << 30)  /* ownership change                  */

/* HcRhPortStatus. The high half is write-1-to-clear change bits, and the low
   half is write-1-to-SET - so this register is written with single action bits
   rather than read-modify-write. Doing the latter sets everything at once. */
#define RH_CCS               (1u << 0)   /* current connect status            */
#define RH_PES               (1u << 1)   /* port enable status                */
#define RH_PSS               (1u << 2)   /* port suspend status               */
#define RH_PRS               (1u << 4)   /* port reset status                 */
#define RH_PPS               (1u << 8)   /* port power status                 */
#define RH_LSDA              (1u << 9)   /* low speed device attached         */
#define RH_CSC               (1u << 16)  /* connect status change             */
#define RH_PRSC              (1u << 20)  /* port reset status change          */
#define RH_SET_PE            (1u << 1)
#define RH_SET_PR            (1u << 4)
#define RH_SET_PP            (1u << 8)

/* HcRhStatus */
#define RHS_LPSC             (1u << 16)  /* set global power                  */

/* ---- descriptors --------------------------------------------------------
 * ED: 16 bytes, 16-byte aligned. TD: 16 bytes, 16-byte aligned. Both are
 * masked by the controller, so the strides below are asserted at build time
 * for the same reason EHCI's qTD is. */
struct ohci_ed {
    volatile uint32_t control;
    volatile uint32_t tail_td;     /* physical; list is empty when head==tail */
    volatile uint32_t head_td;     /* bit 0 = halted, bit 1 = toggle carry    */
    volatile uint32_t next_ed;
} __attribute__((packed));

struct ohci_td {
    volatile uint32_t control;
    volatile uint32_t current_buf; /* advanced by the controller as it works  */
    volatile uint32_t next_td;
    volatile uint32_t buf_end;     /* LAST byte, not one past - an off-by-one
                                      here transfers one byte too few         */
} __attribute__((packed));

_Static_assert(sizeof(struct ohci_ed) == 16, "an ED is 16 bytes");
_Static_assert(sizeof(struct ohci_td) == 16, "a TD is 16 bytes");

/* ED control */
#define ED_FA(a)        ((uint32_t)(a) & 0x7F)          /* function address   */
#define ED_EN(e)        (((uint32_t)(e) & 0x0F) << 7)   /* endpoint number    */
#define ED_DIR_TD       (0u << 11)   /* direction comes from each TD          */
#define ED_SPEED_LOW    (1u << 13)
#define ED_SKIP         (1u << 14)
#define ED_MPS(m)       (((uint32_t)(m) & 0x7FF) << 16)

/* TD control */
#define TD_ROUNDING     (1u << 18)   /* a short packet is not an error        */
#define TD_DP_SETUP     (0u << 19)
#define TD_DP_OUT       (1u << 19)
#define TD_DP_IN        (2u << 19)
#define TD_DI_NONE      (7u << 21)   /* no interrupt delay                    */
#define TD_TOGGLE_DATA0 (2u << 24)   /* bit 25 = use this, bit 24 = the value */
#define TD_TOGGLE_DATA1 (3u << 24)
#define TD_CC(c)        (((c) >> 28) & 0x0F)
#define TD_CC_NOERROR   0
#define TD_CC_NOTACCESSED 14         /* still owned by the controller         */

/* ---- DMA layout ---------------------------------------------------------
 * 0x600000 is the window pmm.c has reserved for OHCI since before it existed -
 * see the comment there. Inside the 0x500000-0x800000 driver DMA range. */
#define ODMA_BASE       0x600000u
#define ODMA_VBASE      0xE5000000u
#define ODMA_V(p)       ((p) - ODMA_BASE + ODMA_VBASE)

#define HCCA_ADDR       (ODMA_BASE + 0x0000)   /* 256 bytes, 256-byte aligned */
#define ED_ADDR         (ODMA_BASE + 0x1000)
#define TD_ADDR         (ODMA_BASE + 0x2000)
#define OXFER_ADDR      (ODMA_BASE + 0x3000)
#define ODMA_END        (ODMA_BASE + 0x8000)

#define OXFER_MAX       4096
#define NUM_TD          4

static volatile uint8_t *regs = 0;
static int present = 0;
static unsigned nports = 0;

static struct ohci_ed *ed;
static struct ohci_td *td;
static uint8_t *xfer_buf;
static uint8_t next_address = 1;

static inline uint32_t rd(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
static inline void wr(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }
static inline uint32_t port_rd(unsigned p) { return rd(OHCI_RHPORTSTATUS + p * 4); }
static inline void port_wr(unsigned p, uint32_t v) { wr(OHCI_RHPORTSTATUS + p * 4, v); }

static uint32_t td_phys(int i) { return TD_ADDR + (uint32_t)i * (uint32_t)sizeof(struct ohci_td); }

/* ---- control transfers --------------------------------------------------
 * Same three stages as every USB control transfer: SETUP (always DATA0), an
 * optional DATA (starting DATA1), and a STATUS in the opposite direction. The
 * TDs are chained and the ED points at the chain; the controller walks from
 * head to tail and stops when they meet. */
static int ohci_control(struct usb_device *dev, uint8_t bm_request_type,
                        uint8_t b_request, uint16_t w_value, uint16_t w_index,
                        void *data, uint16_t w_length) {
    if (!present || !dev) return -1;
    if (w_length > OXFER_MAX) return -1;

    int dev_to_host = (bm_request_type & 0x80) != 0;
    uint8_t mps = dev->max_packet0 ? dev->max_packet0 : 8;

    struct usb_setup *sp = (struct usb_setup *)(xfer_buf + OXFER_MAX);
    sp->bm_request_type = bm_request_type;
    sp->b_request = b_request;
    sp->w_value = w_value;
    sp->w_index = w_index;
    sp->w_length = w_length;
    uint32_t setup_phys = OXFER_ADDR + OXFER_MAX;

    if (!dev_to_host && data && w_length)
        for (uint16_t i = 0; i < w_length; i++) xfer_buf[i] = ((const uint8_t *)data)[i];

    for (int i = 0; i < NUM_TD; i++) {
        td[i].control = 0; td[i].current_buf = 0; td[i].next_td = 0; td[i].buf_end = 0;
    }

    int n = 0;
    struct ohci_td *t_setup = &td[n++];
    t_setup->control = TD_DP_SETUP | TD_TOGGLE_DATA0 | TD_DI_NONE;
    t_setup->current_buf = setup_phys;
    t_setup->buf_end = setup_phys + 8 - 1;          /* LAST byte */

    struct ohci_td *t_data = 0;
    if (w_length) {
        t_data = &td[n++];
        t_data->control = (dev_to_host ? TD_DP_IN : TD_DP_OUT)
                        | TD_TOGGLE_DATA1 | TD_DI_NONE | TD_ROUNDING;
        t_data->current_buf = OXFER_ADDR;
        t_data->buf_end = OXFER_ADDR + w_length - 1;
    }

    struct ohci_td *t_status = &td[n++];
    t_status->control = (dev_to_host ? TD_DP_OUT : TD_DP_IN)
                      | TD_TOGGLE_DATA1 | TD_DI_NONE;
    t_status->current_buf = 0;
    t_status->buf_end = 0;

    /* The tail TD is a real, never-executed descriptor: the controller stops
       when head reaches tail, so the list needs one spare beyond the work. */
    struct ohci_td *t_tail = &td[n];
    t_tail->control = 0; t_tail->current_buf = 0; t_tail->next_td = 0; t_tail->buf_end = 0;

    for (int i = 0; i < n; i++) td[i].next_td = td_phys(i + 1);
    td[n].next_td = 0;

    ed->control = ED_FA(dev->address) | ED_EN(0) | ED_DIR_TD | ED_MPS(mps)
                | (dev->speed == USB_SPEED_LOW ? ED_SPEED_LOW : 0);
    ed->tail_td = td_phys(n);
    ed->next_ed = 0;
    /* head is written LAST and without the halt/toggle bits: writing it is what
       hands the chain to the controller. */
    ed->head_td = td_phys(0);

    wr(OHCI_CTRLHEADED, ED_ADDR);
    wr(OHCI_INTSTATUS, INT_WDH);
    wr(OHCI_CMDSTATUS, CMD_CLF);
    wr(OHCI_CONTROL, rd(OHCI_CONTROL) | CTRL_CLE);

    int err = 0;
    int done = 0;
    for (int ms = 0; ms < 1000; ms++) {
        /* The controller walks head toward tail; they meet when it is finished.
           Bit 0 of head is the halt flag, set when a TD failed. */
        uint32_t head = ed->head_td;
        if (head & 1u) { err = 1; break; }
        if ((head & ~0x0Fu) == (ed->tail_td & ~0x0Fu)) { done = 1; break; }
        timer_sleep(1);
    }
    if (!done) err = 1;

    /* Take the ED back out of the list before touching it again. */
    wr(OHCI_CONTROL, rd(OHCI_CONTROL) & ~CTRL_CLE);
    wr(OHCI_CTRLHEADED, 0);
    ed->head_td = 0;
    ed->tail_td = 0;

    if (!err) {
        uint8_t cc = (uint8_t)TD_CC(t_status->control);
        if (cc != TD_CC_NOERROR) err = 1;
    }
    if (err) return -1;

    int transferred = w_length;
    if (t_data) {
        /* currentBufferPointer is advanced as the transfer runs and is zeroed
           when the TD completes in full. Non-zero means it stopped early. */
        uint32_t cur = t_data->current_buf;
        if (cur) {
            transferred = (int)(cur - OXFER_ADDR);
            if (transferred < 0) transferred = 0;
            if (transferred > (int)w_length) transferred = (int)w_length;
        }
    }

    if (dev_to_host && data && transferred > 0)
        for (int i = 0; i < transferred; i++) ((uint8_t *)data)[i] = xfer_buf[i];

    return transferred;
}

static int ohci_attach(struct usb_device *dev) {
    struct usb_device_descriptor dd;
    for (unsigned i = 0; i < sizeof(dd); i++) ((uint8_t *)&dd)[i] = 0;

    dev->address = 0;
    /* 8 is the only packet size every low- and full-speed device is guaranteed
       to accept before its descriptor has been read. */
    dev->max_packet0 = 8;
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &dd, 8) < 8) return 0;
    dev->max_packet0 = dd.b_max_packet_size0 ? dd.b_max_packet_size0 : 8;

    uint8_t addr = next_address++;
    if (ohci_control(dev, 0x00, USB_REQ_SET_ADDRESS, addr, 0, 0, 0) < 0) return 0;
    dev->address = addr;
    timer_sleep(2);
    return 1;
}

static const struct usb_hc_ops ohci_ops = {
    .name    = "OHCI",
    .attach  = ohci_attach,
    .control = ohci_control,
    .bulk    = 0,
};

static void enumerate_port(unsigned p) {
    uint32_t st = port_rd(p);
    if (!(st & RH_CCS)) return;

    port_wr(p, RH_SET_PR);                  /* single action bit, see the note */
    int ok = 0;
    for (int i = 0; i < 100; i++) {
        timer_sleep(1);
        st = port_rd(p);
        if (st & RH_PRSC) { ok = 1; break; }
    }
    port_wr(p, RH_PRSC);                    /* write-1-to-clear the change */
    if (!ok) {
        klog_u32("OHCI", SEV_WARN, "port ", p, LOG_COLOR_VALUE, ": reset never completed");
        return;
    }
    timer_sleep(10);                        /* the spec's post-reset recovery */

    st = port_rd(p);
    if (!(st & RH_PES)) {
        port_wr(p, RH_SET_PE);
        timer_sleep(5);
        st = port_rd(p);
    }
    if (!(st & RH_PES)) return;

    enum usb_speed sp = (st & RH_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;

    struct usb_device *dev = usb_alloc_device(&ohci_ops, 0, (uint8_t)p, sp);
    if (!dev) { klog("OHCI", SEV_WARN, "device table full"); return; }
    usb_enumerate(dev);
}

/* ---- SMM handoff --------------------------------------------------------
 * OHCI's is simpler than EHCI's and lives in the controller rather than PCI
 * config space: HcControl.IR set means SMM owns it, and writing
 * HcCommandStatus.OCR asks for it back. Same purpose - stop the firmware
 * servicing this controller behind us - and the same conservative timeout,
 * because a firmware that never answers should not leave USB dead. */
static void ohci_takeover(void) {
    uint32_t control = rd(OHCI_CONTROL);

    if (control & CTRL_IR) {
        klog("OHCI", SEV_INFO, "requesting controller ownership from SMM");
        wr(OHCI_INTENABLE, INT_OC);
        wr(OHCI_CMDSTATUS, CMD_OCR);
        for (int i = 0; i < 500; i++) {
            if (!(rd(OHCI_CONTROL) & CTRL_IR)) {
                klog("OHCI", SEV_OK, "SMM released the controller");
                return;
            }
            timer_sleep(1);
        }
        klog("OHCI", SEV_WARN, "SMM did not release the controller - taking it anyway");
        wr(OHCI_CONTROL, rd(OHCI_CONTROL) & ~CTRL_IR);
        return;
    }

    /* No SMM, but the BIOS may still have left it running. Resume rather than
       yank it, then let the reset below do the real work. */
    if ((control & CTRL_HCFS_MASK) != CTRL_HCFS_RESET) {
        if ((control & CTRL_HCFS_MASK) != CTRL_HCFS_OPERATIONAL) {
            wr(OHCI_CONTROL, (control & ~CTRL_HCFS_MASK) | CTRL_HCFS_RESUME);
            timer_sleep(20);
        }
    }
}

int ohci_init(void) {
    present = 0;
    next_address = 1;

    /* class 0x0C serial bus, subclass 0x03 USB, prog-if 0x10 = OHCI */
    const struct pci_device *dev = pci_find(0x0C, 0x03, 0x10, 0);
    if (!dev) return 0;

    uint32_t base = dev->bar[0] & 0xFFFFFFF0u;
    if (base == 0) return 0;

    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    if (base < 0xC0000000u) {
        klog_u32("OHCI", SEV_WARN, "register BAR below the kernel half: ", base,
                 LOG_COLOR_VALUE, " - not visible from a process address space");
        return 0;
    }
    paging_map(base, base, PAGE_PRESENT | PAGE_WRITE | PAGE_NO_CACHE);
    regs = (volatile uint8_t *)base;

    for (uint32_t a = ODMA_BASE; a < ODMA_END; a += 0x1000)
        paging_map(ODMA_V(a), a, PAGE_PRESENT | PAGE_WRITE);

    ohci_takeover();

    /* FmInterval is destroyed by the reset and has to be put back, so save it
       first - the controller will not schedule anything without a sane frame
       interval, and its reset default is not necessarily it. */
    uint32_t fminterval = rd(OHCI_FMINTERVAL);

    wr(OHCI_CMDSTATUS, CMD_HCR);
    int ok = 0;
    for (int i = 0; i < 100; i++) {
        if (!(rd(OHCI_CMDSTATUS) & CMD_HCR)) { ok = 1; break; }
        timer_sleep(1);      /* the spec says 10 us; a millisecond is generous */
    }
    if (!ok) { klog("OHCI", SEV_WARN, "controller reset did not complete"); return 0; }

    /* After reset the controller is in SUSPEND and must reach OPERATIONAL
       within 2 ms or it drops back to RESET. */
    struct ohci_ed *e = (struct ohci_ed *)ODMA_V(ED_ADDR);
    e->control = ED_SKIP;
    e->tail_td = 0; e->head_td = 0; e->next_ed = 0;
    ed = e;
    td = (struct ohci_td *)ODMA_V(TD_ADDR);
    xfer_buf = (uint8_t *)ODMA_V(OXFER_ADDR);

    volatile uint8_t *hcca = (volatile uint8_t *)ODMA_V(HCCA_ADDR);
    for (unsigned i = 0; i < 256; i++) hcca[i] = 0;

    wr(OHCI_HCCA, HCCA_ADDR);
    wr(OHCI_CTRLHEADED, 0);
    wr(OHCI_BULKHEADED, 0);
    wr(OHCI_FMINTERVAL, fminterval);
    wr(OHCI_PERIODICSTART, (fminterval & 0x3FFF) * 9 / 10);   /* 90%, per the spec */
    wr(OHCI_INTDISABLE, 0xFFFFFFFF);                          /* we poll */

    wr(OHCI_CONTROL, (rd(OHCI_CONTROL) & ~(CTRL_HCFS_MASK | CTRL_IR))
                     | CTRL_HCFS_OPERATIONAL);

    uint32_t rhda = rd(OHCI_RHDESCRIPTORA);
    nports = rhda & 0xFF;
    if (nports == 0 || nports > 15) {
        klog_u32("OHCI", SEV_WARN, "implausible root port count ", nports, LOG_COLOR_VALUE, "");
        return 0;
    }

    /* Global power on, then per-port. POTPGT (the top byte of HcRhDescriptorA)
       is the power-on-to-power-good time in 2 ms units, and devices are not
       required to answer before it elapses. */
    wr(OHCI_RHSTATUS, RHS_LPSC);
    for (unsigned p = 0; p < nports; p++) port_wr(p, RH_SET_PP);
    unsigned potpgt = ((rhda >> 24) & 0xFF) * 2;
    timer_sleep(potpgt < 20 ? 20 : potpgt);

    present = 1;
    klog_u32("OHCI", SEV_OK, "controller online, root ports: ", nports, LOG_COLOR_VALUE, "");

    timer_sleep(100);
    for (unsigned p = 0; p < nports; p++) enumerate_port(p);
    return 1;
}
