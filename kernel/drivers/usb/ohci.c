// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/usb/ohci.c */
/* Aurora Tejeda / CATX Systems */
/* OHCI host controller. See ohci.h for why this is the one the AM3+ board
   needs for its keyboard and mouse. */

#include "ohci.h"
#include "usb.h"
#include "memman/dma.h"
#include "pci.h"
#include "arch/paging.h"
#include "logging.h"
#include "timer.h"
#include "drivers/iowin.h"
#include "format.h"

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

/* ---- DMA memory ---------------------------------------------------------
 * Each controller has its own eight pages (dma_alloc), physical for the
 * controller and a kernel-half alias for the CPU. The offsets below are within
 * those pages. */
#define ODMA_VBASE      0xE5000000u     /* controller i's alias: + i * 0x10000 */
#define ODMA_PAGES      8

#define HCCA_OFF        0x0000          /* 256 bytes, 256-byte aligned */
#define ED_OFF          0x1000
/* One bulk ED per DIRECTION, kept alive between transfers. OHCI carries the
   data toggle in the ED (headTD bit 1, "toggleCarry") and advances it itself,
   which is exactly what a bulk endpoint needs - but only while the ED survives.
   The control ED has its head rewritten every transfer, which would reset the
   carry, so bulk cannot share it. IN and OUT are separate endpoints with
   separate toggles, hence two. */
#define ED_BULK_IN      0x1040
#define ED_BULK_OUT     0x1080
/* One interrupt ED per HID endpoint, chained together on the HCCA periodic
   list. A single shared ED looked fine with one device and silently broke the
   moment a keyboard and a mouse were both attached - they overwrote each
   other's in-flight transfer. */
#define MAX_INT_EPS     4
#define ED_INT(i)       (0x1100 + (uint32_t)(i) * 0x40)
#define INT_TD(i)       (0x2400 + (uint32_t)(i) * 0x80)
#define INT_BUF(i)      (0x4000 + (uint32_t)(i) * 0x100)
#define TD_OFF          0x2000
#define OXFER_OFF       0x3000

#define OXFER_MAX       4096
#define NUM_TD          4

/* An AMD SB950 has five OHCI controllers: one companion per EHCI, and one on
   its own. A full- or low-speed device is reachable only through the companion
   of the EHCI whose port it is in, so every one has to come up. */
#define OHCI_MAX_HCS    6

/* Everything one controller needs; a usb_device's `hc` points at its
   controller's. */
struct ohci_hc {
    struct iowin regs;              /* BAR 0: the 4 KB operational registers */
    unsigned nports;
    uint8_t next_address;           /* USB addresses are per bus */

    uint32_t dma;                   /* physical base of the DMA pages */
    uint8_t *dma_v;                 /* the CPU's alias of them        */
    struct ohci_ed *ed;
    struct ohci_td *td;
    uint8_t *xfer_buf;

    struct {
        uint8_t addr, ep, armed, len;
    } int_eps[MAX_INT_EPS];
    int int_list_live;
    uint16_t seen;                  /* ports already found connected and tried */
};

static struct ohci_hc hcs[OHCI_MAX_HCS];
static unsigned nhcs = 0;

static inline uint32_t dma_phys(const struct ohci_hc *c, uint32_t off) { return c->dma + off; }
static inline void    *dma_virt(const struct ohci_hc *c, uint32_t off) { return c->dma_v + off; }

static inline uint32_t rd(struct ohci_hc *c, uint32_t o) { return iowin_read32(&c->regs, o); }
static inline void wr(struct ohci_hc *c, uint32_t o, uint32_t v) { iowin_write32(&c->regs, o, v); }
static inline uint32_t port_rd(struct ohci_hc *c, unsigned p) { return rd(c, OHCI_RHPORTSTATUS + p * 4); }
static inline void port_wr(struct ohci_hc *c, unsigned p, uint32_t v) { wr(c, OHCI_RHPORTSTATUS + p * 4, v); }

static uint32_t td_phys(const struct ohci_hc *c, int i) {
    return dma_phys(c, TD_OFF) + (uint32_t)i * (uint32_t)sizeof(struct ohci_td);
}

/* ---- control transfers --------------------------------------------------
 * Same three stages as every USB control transfer: SETUP (always DATA0), an
 * optional DATA (starting DATA1), and a STATUS in the opposite direction. The
 * TDs are chained and the ED points at the chain; the controller walks from
 * head to tail and stops when they meet. */
static int ohci_control(struct usb_device *dev, uint8_t bm_request_type,
                        uint8_t b_request, uint16_t w_value, uint16_t w_index,
                        void *data, uint16_t w_length) {
    if (!dev || !dev->hc) return -1;
    if (w_length > OXFER_MAX) return -1;
    struct ohci_hc *c = dev->hc;
    struct ohci_ed *ed = c->ed;
    struct ohci_td *td = c->td;
    uint8_t *xfer_buf = c->xfer_buf;
    uint32_t xfer_phys = dma_phys(c, OXFER_OFF);

    int dev_to_host = (bm_request_type & 0x80) != 0;
    uint8_t mps = dev->max_packet0 ? dev->max_packet0 : 8;

    struct usb_setup *sp = (struct usb_setup *)(xfer_buf + OXFER_MAX);
    sp->bm_request_type = bm_request_type;
    sp->b_request = b_request;
    sp->w_value = w_value;
    sp->w_index = w_index;
    sp->w_length = w_length;
    uint32_t setup_phys = xfer_phys + OXFER_MAX;

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
        t_data->current_buf = xfer_phys;
        t_data->buf_end = xfer_phys + w_length - 1;
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

    for (int i = 0; i < n; i++) td[i].next_td = td_phys(c, i + 1);
    td[n].next_td = 0;

    ed->control = ED_FA(dev->address) | ED_EN(0) | ED_DIR_TD | ED_MPS(mps)
                | (dev->speed == USB_SPEED_LOW ? ED_SPEED_LOW : 0);
    ed->tail_td = td_phys(c, n);
    ed->next_ed = 0;
    /* head is written LAST and without the halt/toggle bits: writing it is what
       hands the chain to the controller. */
    ed->head_td = td_phys(c, 0);

    wr(c, OHCI_CTRLHEADED, dma_phys(c, ED_OFF));
    wr(c, OHCI_INTSTATUS, INT_WDH);
    wr(c, OHCI_CMDSTATUS, CMD_CLF);
    wr(c, OHCI_CONTROL, rd(c, OHCI_CONTROL) | CTRL_CLE);

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
    wr(c, OHCI_CONTROL, rd(c, OHCI_CONTROL) & ~CTRL_CLE);
    wr(c, OHCI_CTRLHEADED, 0);
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
            transferred = (int)(cur - xfer_phys);
            if (transferred < 0) transferred = 0;
            if (transferred > (int)w_length) transferred = (int)w_length;
        }
    }

    if (dev_to_host && data && transferred > 0)
        for (int i = 0; i < transferred; i++) ((uint8_t *)data)[i] = xfer_buf[i];

    return transferred;
}

/* ---- bulk transfers -----------------------------------------------------
 * The same TD machinery as a control transfer, on the bulk list instead of the
 * control list, and with the toggle left to the ED. A TD whose toggle field has
 * bit 25 clear means "use the ED's toggleCarry", which the controller then
 * advances - so the persistence a bulk endpoint requires is free here, unlike
 * on EHCI where it has to be saved and restored by hand. */
static int ohci_bulk(struct usb_device *dev, uint8_t ep, void *data, uint32_t len) {
    if (!dev || !dev->hc || !data) return -1;
    if (len == 0 || len > OXFER_MAX) return -1;
    struct ohci_hc *c = dev->hc;
    struct ohci_td *td = c->td;
    uint8_t *xfer_buf = c->xfer_buf;
    uint32_t xfer_phys = dma_phys(c, OXFER_OFF);

    int in = (ep & 0x80) != 0;
    uint16_t mps = in ? dev->ep_in_mps : dev->ep_out_mps;
    if (mps == 0) mps = 64;

    uint32_t bed_off = in ? ED_BULK_IN : ED_BULK_OUT;
    struct ohci_ed *bed = dma_virt(c, bed_off);

    if (!in) for (uint32_t i = 0; i < len; i++) xfer_buf[i] = ((const uint8_t *)data)[i];

    for (int i = 0; i < NUM_TD; i++) {
        td[i].control = 0; td[i].current_buf = 0; td[i].next_td = 0; td[i].buf_end = 0;
    }

    struct ohci_td *t = &td[0];
    t->control = (in ? TD_DP_IN : TD_DP_OUT) | TD_DI_NONE | TD_ROUNDING;  /* toggle from the ED */
    t->current_buf = xfer_phys;
    t->buf_end = xfer_phys + len - 1;           /* LAST byte */
    t->next_td = td_phys(c, 1);

    struct ohci_td *t_tail = &td[1];
    t_tail->control = 0; t_tail->current_buf = 0; t_tail->next_td = 0; t_tail->buf_end = 0;

    /* Preserve the toggle carry the controller left in headTD bit 1. */
    uint32_t carry = bed->head_td & 2u;
    bed->control = ED_FA(dev->address) | ED_EN(ep & 0x0F) | ED_DIR_TD | ED_MPS(mps)
                 | (dev->speed == USB_SPEED_LOW ? ED_SPEED_LOW : 0);
    bed->tail_td = td_phys(c, 1);
    bed->next_ed = 0;
    bed->head_td = td_phys(c, 0) | carry;

    wr(c, OHCI_BULKHEADED, dma_phys(c, bed_off));
    wr(c, OHCI_CMDSTATUS, CMD_BLF);
    wr(c, OHCI_CONTROL, rd(c, OHCI_CONTROL) | CTRL_BLE);

    int err = 0, done = 0;
    for (int ms = 0; ms < 2000; ms++) {
        uint32_t head = bed->head_td;
        if (head & 1u) { err = 1; break; }       /* halted */
        if ((head & ~0x0Fu) == (bed->tail_td & ~0x0Fu)) { done = 1; break; }
        timer_sleep(1);
    }
    if (!done) err = 1;

    wr(c, OHCI_CONTROL, rd(c, OHCI_CONTROL) & ~CTRL_BLE);
    wr(c, OHCI_BULKHEADED, 0);

    if (!err && TD_CC(t->control) != TD_CC_NOERROR) err = 1;
    if (err) return -1;

    int transferred = (int)len;
    uint32_t cur = t->current_buf;
    if (cur) {
        transferred = (int)(cur - xfer_phys);
        if (transferred < 0) transferred = 0;
        if (transferred > (int)len) transferred = (int)len;
    }
    if (in && transferred > 0)
        for (int i = 0; i < transferred; i++) ((uint8_t *)data)[i] = xfer_buf[i];
    return transferred;
}

/* Stop the bulk list, so a TD a panic interrupted does not run on, and drop
   the ED's toggle carry to match a device whose stall was cleared (usb.h).
   The 2 ms lets the controller finish the frame it was in. Control transfers
   rebuild their ED from scratch each time, so `ep` 0 needs nothing. */
static void ohci_reset_ep(struct usb_device *dev, uint8_t ep) {
    if (!dev || !dev->hc || !(ep & 0x0F)) return;
    struct ohci_hc *c = dev->hc;
    wr(c, OHCI_CONTROL, rd(c, OHCI_CONTROL) & ~CTRL_BLE);
    wr(c, OHCI_BULKHEADED, 0);
    wr(c, OHCI_BULKCURRENTED, 0);
    timer_sleep(2);
    struct ohci_ed *bed = dma_virt(c, (ep & 0x80) ? ED_BULK_IN : ED_BULK_OUT);
    bed->head_td = 0;
}

static int ohci_attach(struct usb_device *dev) {
    struct ohci_hc *c = dev->hc;
    struct usb_device_descriptor dd;
    for (unsigned i = 0; i < sizeof(dd); i++) ((uint8_t *)&dd)[i] = 0;

    dev->address = 0;
    /* 8 is the only packet size every low- and full-speed device is guaranteed
       to accept before its descriptor has been read. */
    dev->max_packet0 = 8;
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &dd, 8) < 8) return 0;
    dev->max_packet0 = dd.b_max_packet_size0 ? dd.b_max_packet_size0 : 8;

    uint8_t addr = c->next_address++;
    if (ohci_control(dev, 0x00, USB_REQ_SET_ADDRESS, addr, 0, 0, 0) < 0) return 0;
    dev->address = addr;
    timer_sleep(2);
    return 1;
}

/* Defined below, after the periodic-schedule helpers it needs. */
static int ohci_interrupt_poll(struct usb_device *dev, uint8_t ep,
                               void *data, uint32_t len);

static const struct usb_hc_ops ohci_ops = {
    .name    = "OHCI",
    .attach  = ohci_attach,
    .control = ohci_control,
    .bulk    = ohci_bulk,
    .interrupt_poll = ohci_interrupt_poll,
    .reset_ep = ohci_reset_ep,
};

static void enumerate_port(struct ohci_hc *c, unsigned p) {
    uint32_t st = port_rd(c, p);
    if (!(st & RH_CCS)) return;
    c->seen |= (uint16_t)(1u << p);         /* tried once, whatever comes of it */
    port_wr(c, p, RH_CSC);                  /* this connection is handled: see ohci_poll_ports */

    port_wr(c, p, RH_SET_PR);              /* single action bit, see the note */
    int ok = 0;
    for (int i = 0; i < 100; i++) {
        timer_sleep(1);
        st = port_rd(c, p);
        if (st & RH_PRSC) { ok = 1; break; }
    }
    port_wr(c, p, RH_PRSC);                 /* write-1-to-clear the change */
    if (!ok) {
        klog_u32("OHCI", SEV_WARN, "port ", p, LOG_COLOR_VALUE, ": reset never completed");
        return;
    }
    timer_sleep(10);                        /* the spec's post-reset recovery */

    st = port_rd(c, p);
    if (!(st & RH_PES)) {
        port_wr(c, p, RH_SET_PE);
        timer_sleep(5);
        st = port_rd(c, p);
    }
    if (!(st & RH_PES)) return;

    enum usb_speed sp = (st & RH_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;

    struct usb_device *dev = usb_alloc_device(&ohci_ops, c, (uint8_t)p, sp);
    if (!dev) { klog("OHCI", SEV_WARN, "device table full"); return; }
    usb_enumerate(dev);
}

/* ---- interrupt transfers ------------------------------------------------
 * These run on the PERIODIC schedule rather than the control or bulk lists: the
 * HCCA holds 32 interrupt list heads, one per frame, and the controller walks
 * the one matching the current frame every millisecond. Pointing all 32 at the
 * same ED polls it every frame, which is the simplest correct arrangement and
 * fast enough for a keyboard.
 *
 * The transfer is armed once and left running. That is the whole point of the
 * non-blocking contract: an idle keyboard NAKs, the TD simply stays active, and
 * a poll that waited for it would block for its timeout on every keystroke
 * check. Instead each poll asks "has it completed yet", and re-arms if so.
 *
 * Its own TDs and buffer, because a control or bulk transfer running between
 * two polls would otherwise overwrite the report in flight. */

/* One slot per (device address, endpoint). Claimed on first poll. */
static int int_slot_for(struct ohci_hc *c, uint8_t addr, uint8_t ep) {
    for (int i = 0; i < MAX_INT_EPS; i++)
        if (c->int_eps[i].armed && c->int_eps[i].addr == addr && c->int_eps[i].ep == ep) return i;
    for (int i = 0; i < MAX_INT_EPS; i++)
        if (!c->int_eps[i].armed) { c->int_eps[i].addr = addr; c->int_eps[i].ep = ep; return i; }
    return -1;
}

static void arm_interrupt(struct ohci_hc *c, struct usb_device *dev, int i, uint8_t ep,
                          uint32_t len) {
    struct ohci_ed *ied = dma_virt(c, ED_INT(i));
    struct ohci_td *t = dma_virt(c, INT_TD(i));
    struct ohci_td *tail = t + 1;
    uint32_t t_phys = dma_phys(c, INT_TD(i));
    uint32_t buf = dma_phys(c, INT_BUF(i));

    t->control = TD_DP_IN | TD_DI_NONE | TD_ROUNDING;   /* toggle from the ED */
    t->current_buf = buf;
    t->buf_end = buf + len - 1;                         /* LAST byte */
    t->next_td = t_phys + sizeof(struct ohci_td);

    tail->control = 0; tail->current_buf = 0; tail->next_td = 0; tail->buf_end = 0;

    uint16_t mps = dev->ep_int_mps ? dev->ep_int_mps : 8;
    uint32_t carry = ied->head_td & 2u;
    /* next_ed is preserved: it is what keeps this ED in the periodic chain. */
    ied->control = ED_FA(dev->address) | ED_EN(ep & 0x0F) | ED_DIR_TD | ED_MPS(mps)
                 | (dev->speed == USB_SPEED_LOW ? ED_SPEED_LOW : 0);
    ied->tail_td = t_phys + sizeof(struct ohci_td);
    ied->head_td = t_phys | carry;

    c->int_eps[i].len = (uint8_t)len;
    c->int_eps[i].armed = 1;
}

static int ohci_interrupt_poll(struct usb_device *dev, uint8_t ep,
                               void *data, uint32_t len) {
    if (!dev || !dev->hc || !data) return -1;
    if (len == 0 || len > 64) return -1;
    struct ohci_hc *c = dev->hc;

    int i = int_slot_for(c, dev->address, ep);
    if (i < 0) return -1;

    struct ohci_ed *ied = dma_virt(c, ED_INT(i));
    struct ohci_td *t = dma_virt(c, INT_TD(i));

    if (!c->int_list_live) {
        /* Chain every interrupt ED together, skipped until armed, and publish
           the head into all 32 HCCA interrupt entries. Until PLE is set the
           controller never looks at the periodic schedule at all. */
        for (int k = 0; k < MAX_INT_EPS; k++) {
            struct ohci_ed *e = dma_virt(c, ED_INT(k));
            e->control = ED_SKIP;
            e->head_td = 0;
            e->tail_td = 0;
            e->next_ed = (k + 1 < MAX_INT_EPS) ? dma_phys(c, ED_INT(k + 1)) : 0;
        }
        volatile uint32_t *hcca = dma_virt(c, HCCA_OFF);
        for (int k = 0; k < 32; k++) hcca[k] = dma_phys(c, ED_INT(0));
        wr(c, OHCI_CONTROL, rd(c, OHCI_CONTROL) | CTRL_PLE);
        c->int_list_live = 1;
    }

    if (!c->int_eps[i].armed) { arm_interrupt(c, dev, i, ep, len); return 0; }

    uint32_t head = ied->head_td;
    if (head & 1u) {                       /* halted - clear and re-arm */
        ied->head_td = 0;
        arm_interrupt(c, dev, i, ep, len);
        return -1;
    }
    if ((head & ~0x0Fu) != (ied->tail_td & ~0x0Fu)) return 0;   /* still running */

    int got = (int)c->int_eps[i].len;
    uint32_t cur = t->current_buf;
    if (cur) {                             /* short report */
        got = (int)(cur - dma_phys(c, INT_BUF(i)));
        if (got < 0) got = 0;
    }
    if (TD_CC(t->control) != TD_CC_NOERROR) got = 0;

    if (got > (int)len) got = (int)len;
    const uint8_t *src = dma_virt(c, INT_BUF(i));
    for (int b = 0; b < got; b++) ((uint8_t *)data)[b] = src[b];

    arm_interrupt(c, dev, i, ep, len);     /* keep it running */
    return got;
}

/* ---- SMM handoff --------------------------------------------------------
 * OHCI's is simpler than EHCI's and lives in the controller rather than PCI
 * config space: HcControl.IR set means SMM owns it, and writing
 * HcCommandStatus.OCR asks for it back. Same purpose - stop the firmware
 * servicing this controller behind us - and the same conservative timeout,
 * because a firmware that never answers should not leave USB dead. */
static void ohci_takeover(struct ohci_hc *c) {
    uint32_t control = rd(c, OHCI_CONTROL);

    if (control & CTRL_IR) {
        klog("OHCI", SEV_INFO, "requesting controller ownership from SMM");
        wr(c, OHCI_INTENABLE, INT_OC);
        wr(c, OHCI_CMDSTATUS, CMD_OCR);
        for (int i = 0; i < 500; i++) {
            if (!(rd(c, OHCI_CONTROL) & CTRL_IR)) {
                klog("OHCI", SEV_OK, "SMM released the controller");
                return;
            }
            timer_sleep(1);
        }
        klog("OHCI", SEV_WARN, "SMM did not release the controller - taking it anyway");
        wr(c, OHCI_CONTROL, rd(c, OHCI_CONTROL) & ~CTRL_IR);
        return;
    }

    /* No SMM, but the BIOS may still have left it running. Resume rather than
       yank it, then let the reset below do the real work. */
    if ((control & CTRL_HCFS_MASK) != CTRL_HCFS_RESET) {
        if ((control & CTRL_HCFS_MASK) != CTRL_HCFS_OPERATIONAL) {
            wr(c, OHCI_CONTROL, (control & ~CTRL_HCFS_MASK) | CTRL_HCFS_RESUME);
            timer_sleep(20);
        }
    }
}

/* Bring up one controller. Returns 1 if it came up. */
static int ohci_start(struct ohci_hc *c, const struct pci_device *dev, unsigned index) {
    pci_enable_bus_master(dev);

    uint32_t base = pci_bar_mmio32(dev, 0, "OHCI");
    if (base == 0) return 0;

    volatile uint8_t *mmio = (volatile uint8_t *)paging_map_phys(base, 0x1000, VM_WRITE | VM_UNCACHED);
    if (!mmio || iowin_mmio(&c->regs, mmio, 0x1000) != 0) return 0;

    c->dma_v = dma_alloc(ODMA_PAGES, ODMA_VBASE + index * 0x10000u, &c->dma, "ohci");
    if (!c->dma_v) {
        klog("OHCI", SEV_WARN, "no memory for the controller's DMA pages");
        return 0;
    }

    ohci_takeover(c);

    /* Hold the bus in USB reset before resetting the controller. The spec does
       not ask for it, but Haiku and FreeBSD both do it after taking a
       controller from the BIOS, because some do not start without it (Haiku's
       ohci.cpp, "without it some controllers don't start"). It also resets
       whatever the BIOS left configured on the ports. */
    wr(c, OHCI_CONTROL, CTRL_HCFS_RESET);
    timer_sleep(100);

    /* FmInterval is destroyed by the reset and has to be put back, so save it
       first - the controller will not schedule anything without a sane frame
       interval, and its reset default is not necessarily it. */
    uint32_t fminterval = rd(c, OHCI_FMINTERVAL);

    wr(c, OHCI_CMDSTATUS, CMD_HCR);
    int ok = 0;
    for (int i = 0; i < 100; i++) {
        if (!(rd(c, OHCI_CMDSTATUS) & CMD_HCR)) { ok = 1; break; }
        timer_sleep(1);      /* the spec says 10 us; a millisecond is generous */
    }
    if (!ok) { klog("OHCI", SEV_WARN, "controller reset did not complete"); return 0; }

    /* After reset the controller is in SUSPEND and must reach OPERATIONAL
       within 2 ms or it drops back to RESET. */
    struct ohci_ed *e = dma_virt(c, ED_OFF);
    e->control = ED_SKIP;
    e->tail_td = 0; e->head_td = 0; e->next_ed = 0;
    c->ed = e;
    for (uint32_t a = ED_BULK_IN; a <= ED_BULK_OUT; a += 0x40) {
        struct ohci_ed *b = dma_virt(c, a);
        b->control = ED_SKIP; b->tail_td = 0; b->head_td = 0; b->next_ed = 0;
    }
    c->td = dma_virt(c, TD_OFF);
    c->xfer_buf = dma_virt(c, OXFER_OFF);

    volatile uint8_t *hcca = dma_virt(c, HCCA_OFF);
    for (unsigned i = 0; i < 256; i++) hcca[i] = 0;

    wr(c, OHCI_HCCA, dma_phys(c, HCCA_OFF));
    wr(c, OHCI_CTRLHEADED, 0);
    wr(c, OHCI_BULKHEADED, 0);
    wr(c, OHCI_FMINTERVAL, fminterval);
    wr(c, OHCI_PERIODICSTART, (fminterval & 0x3FFF) * 9 / 10);   /* 90%, per the spec */
    wr(c, OHCI_INTDISABLE, 0xFFFFFFFF);                          /* we poll */

    wr(c, OHCI_CONTROL, (rd(c, OHCI_CONTROL) & ~(CTRL_HCFS_MASK | CTRL_IR))
                        | CTRL_HCFS_OPERATIONAL);

    uint32_t rhda = rd(c, OHCI_RHDESCRIPTORA);
    c->nports = rhda & 0xFF;
    if (c->nports == 0 || c->nports > 15) {
        klog_u32("OHCI", SEV_WARN, "implausible root port count ", c->nports, LOG_COLOR_VALUE, "");
        return 0;
    }

    /* Global power on, then per-port. POTPGT (the top byte of HcRhDescriptorA)
       is the power-on-to-power-good time in 2 ms units, and devices are not
       required to answer before it elapses. */
    wr(c, OHCI_RHSTATUS, RHS_LPSC);
    for (unsigned p = 0; p < c->nports; p++) port_wr(c, p, RH_SET_PP);
    unsigned potpgt = ((rhda >> 24) & 0xFF) * 2;
    timer_sleep(potpgt < 20 ? 20 : potpgt);

    klog_u32("OHCI", SEV_OK, "controller online, root ports: ", c->nports, LOG_COLOR_VALUE, "");
    klog_child_u32("  pci device ", dev->slot, LOG_COLOR_VALUE, "");

    timer_sleep(100);
    for (unsigned p = 0; p < c->nports; p++) enumerate_port(c, p);
    return 1;
}

/* A removed device's interrupt slots: skip its EDs and free the slots, or
   four replugs would use them all up and the fifth keyboard get none. */
static void release_int_slots(struct ohci_hc *c, uint8_t addr) {
    for (int i = 0; i < MAX_INT_EPS; i++) {
        if (!c->int_eps[i].armed || c->int_eps[i].addr != addr) continue;
        struct ohci_ed *ied = dma_virt(c, ED_INT(i));
        ied->control |= ED_SKIP;
        c->int_eps[i].armed = 0;
    }
}

void ohci_poll_ports(void) {
    for (unsigned h = 0; h < nhcs; h++) {
        struct ohci_hc *c = &hcs[h];
        for (unsigned p = 0; p < c->nports; p++) {
            /* The change bit, not the connect bit: a device can go and come
               back between two looks, and then only the change bit says so.
               enumerate_port clears it, so a device found at boot is not
               taken for a new one here. */
            uint32_t st = port_rd(c, p);
            if (!(st & RH_CSC)) continue;                  /* nothing since the last look */
            port_wr(c, p, RH_CSC);                         /* write-1-to-clear */
            uint16_t bit = (uint16_t)(1u << p);
            if (c->seen & bit) {                           /* whatever was there is gone */
                c->seen &= (uint16_t)~bit;
                struct usb_device *d = usb_find(c, (uint8_t)p);
                if (d) {
                    release_int_slots(c, d->address);
                    usb_port_gone(d);
                }
            }
            if (st & RH_CCS) {
                timer_sleep(100);                          /* connect debounce (USB 2.0 7.1.7.3) */
                enumerate_port(c, p);
            }
        }
    }
}

void ohci_report(void) {
    for (unsigned h = 0; h < nhcs; h++) {
        struct ohci_hc *c = &hcs[h];
        /* "OHCI 0 ports: 00000100 00000303 ..." three to a line, to fit the
           log's 48 columns. */
        for (unsigned p0 = 0; p0 < c->nports; p0 += 3) {
            char line[48];
            int n = 0;
            for (const char *s = "OHCI "; *s; s++) line[n++] = *s;
            n += (int)fmt_u32(line + n, h);
            for (const char *s = p0 ? " more:" : " ports:"; *s; s++) line[n++] = *s;
            for (unsigned p = p0; p < c->nports && p < p0 + 3; p++) {
                line[n++] = ' ';
                n += (int)fmt_hex(line + n, port_rd(c, p), 8, 0);
            }
            line[n] = 0;
            klog_child(line);
        }
    }
}

int ohci_init(void) {
    nhcs = 0;
    /* class 0x0C serial bus, subclass 0x03 USB, prog-if 0x10 = OHCI. Every
       one of them: each is the companion for a different set of ports. */
    for (unsigned k = 0; nhcs < OHCI_MAX_HCS; k++) {
        const struct pci_device *dev = pci_find(0x0C, 0x03, 0x10, k);
        if (!dev) break;
        struct ohci_hc *c = &hcs[nhcs];
        for (unsigned i = 0; i < sizeof(*c); i++) ((uint8_t *)c)[i] = 0;
        c->next_address = 1;
        if (ohci_start(c, dev, nhcs)) nhcs++;
    }
    return (int)nhcs;
}
