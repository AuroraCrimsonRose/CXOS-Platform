// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/usb/xhci.c */
/* Aurora Tejeda / CATX Systems */
/* xHCI host controller. See xhci.h for why this one is structurally different
   from EHCI and OHCI rather than merely newer. */

#include "xhci.h"
#include "usb.h"
#include "pci.h"
#include "arch/paging.h"
#include "logging.h"
#include "arch/irq.h"
#include "timer.h"
#include "drivers/iowin.h"
#include "xhci_spad.h"

/* ---- capability registers (at BAR0) ------------------------------------- */
#define XCAP_CAPLENGTH   0x00   /* 8-bit  */
#define XCAP_HCSPARAMS1  0x04
#define XCAP_HCSPARAMS2  0x08
#define XCAP_HCCPARAMS1  0x10
#define XCAP_DBOFF       0x14   /* doorbell array offset from BAR0           */
#define XCAP_RTSOFF      0x18   /* runtime register offset from BAR0         */

#define HCS1_MAXSLOTS(x) ((x) & 0xFF)
#define HCS1_MAXPORTS(x) (((x) >> 24) & 0xFF)
/* Scratchpad count is split across two non-adjacent fields, which is easy to
   get wrong and fails as a controller that will not start. */
#define HCC1_CSZ(x)      (((x) >> 2) & 1)   /* 1 = 64-byte contexts          */

/* ---- operational registers (at BAR0 + CAPLENGTH) ------------------------ */
#define XOP_USBCMD       0x00
#define XOP_USBSTS       0x04
#define XOP_PAGESIZE     0x08   /* bit n: pages of 2^(n+12) bytes supported   */
#define XOP_CRCR         0x18
#define XOP_DCBAAP       0x30
#define XOP_CONFIG       0x38
#define XOP_PORTSC_BASE  0x400  /* port n (1-based) at BASE + (n-1) * 0x10   */

#define XCMD_RS          (1u << 0)
#define XCMD_HCRST       (1u << 1)

#define XSTS_HCH         (1u << 0)   /* halted                              */
#define XSTS_CNR         (1u << 11)  /* controller not ready                */

/* PORTSC. Bits 17-23 are write-1-to-clear, and bit 1 (PED) is write-1-to-
   DISABLE - so a naive read-modify-write disables the port and wipes the
   change flags at the same time. Every write goes through portsc_wr(). */
#define XPORT_CCS        (1u << 0)
#define XPORT_PED        (1u << 1)
#define XPORT_PR         (1u << 4)
#define XPORT_PP         (1u << 9)
#define XPORT_SPEED(x)   (((x) >> 10) & 0x0F)
#define XPORT_CSC        (1u << 17)
#define XPORT_PRC        (1u << 21)
#define XPORT_RW1C_MASK  0x00FE0000u
#define XPORT_PRESERVE   (~(XPORT_PED | XPORT_RW1C_MASK))

/* Default speed IDs, xHCI spec table 157. A controller may remap these via the
   Supported Protocol extended capability; none of the common ones do. */
#define XSPEED_FULL  1
#define XSPEED_LOW   2
#define XSPEED_HIGH  3
#define XSPEED_SUPER 4

/* ---- TRBs --------------------------------------------------------------- */
struct xhci_trb {
    volatile uint64_t parameter;
    volatile uint32_t status;
    volatile uint32_t control;
} __attribute__((packed));

_Static_assert(sizeof(struct xhci_trb) == 16, "a TRB is 16 bytes");

#define TRB_TYPE(t)      (((uint32_t)(t)) << 10)
#define TRB_GET_TYPE(c)  (((c) >> 10) & 0x3F)
#define TRB_CYCLE        (1u << 0)
#define TRB_ENT          (1u << 1)
#define TRB_ISP          (1u << 2)
#define TRB_CH           (1u << 4)
#define TRB_IOC          (1u << 5)
#define TRB_IDT          (1u << 6)   /* parameter holds data, not a pointer  */

#define TRB_NORMAL        1
#define TRB_SETUP_STAGE   2
#define TRB_DATA_STAGE    3
#define TRB_STATUS_STAGE  4
#define TRB_LINK          6
#define TRB_CMD_NOOP         23
#define TRB_CMD_ENABLE_SLOT   9
#define TRB_CMD_ADDRESS_DEV  11
#define TRB_CMD_CONFIG_EP    12
#define TRB_CMD_EVAL_CTX     13
#define TRB_CMD_RESET_EP     14
#define TRB_CMD_STOP_EP      15
#define TRB_CMD_SET_TR_DEQ   16
#define TRB_EV_TRANSFER      32
#define TRB_EV_CMD_COMPLETE  33
#define TRB_EV_PORT_STATUS   34

#define TRB_CC(status)   (((status) >> 24) & 0xFF)
/* A Transfer Event names where it came from: slot in the top byte of control,
   endpoint DCI in bits 16-20. */
#define TRB_EV_SLOT(ctrl) (((ctrl) >> 24) & 0xFF)
#define TRB_EV_DCI(ctrl)  (((ctrl) >> 16) & 0x1F)
#define TRB_CC_SUCCESS   1
#define TRB_CC_SHORT_PKT 13

/* ---- DMA layout ---------------------------------------------------------
 * Same arrangement as the other DMA drivers: physical for the controller, a
 * kernel-half alias for the CPU so the structures stay reachable from a process
 * address space. 0x770000 is inside the 0x500000-0x800000 window pmm.c reserves
 * for driver DMA, past EHCI which ends at 0x768000.
 *
 * xHCI's alignment rules are strict and silent when violated: the DCBAA, the
 * command ring, the ERST and every context must be 64-byte aligned. Page
 * granularity below satisfies all of that with room to spare. */
#define XDMA_BASE       0x770000u
#define XDMA_VBASE      0xE4000000u
#define XDMA_V(p)       ((p) - XDMA_BASE + XDMA_VBASE)

#define DCBAA_ADDR      (XDMA_BASE + 0x0000)
#define CMDRING_ADDR    (XDMA_BASE + 0x1000)
#define EVTRING_ADDR    (XDMA_BASE + 0x2000)
#define ERST_ADDR       (XDMA_BASE + 0x3000)
#define INPUTCTX_ADDR   (XDMA_BASE + 0x4000)
#define DEVCTX_ADDR     (XDMA_BASE + 0x5000)   /* one page per slot          */
#define EP0RING_ADDR    (XDMA_BASE + 0xD000)   /* 256 bytes per slot         */
#define XFERBUF_ADDR    (XDMA_BASE + 0xE000)
/* 0xF000-0x1FFFF: free (the scratchpad pages come from the page allocator) */
#define BULKRING_ADDR   (XDMA_BASE + 0x20000)  /* 256 bytes per slot per dir */
/* Its own ring rather than sharing the bulk IN one: a composite device with
   both a bulk and an interrupt IN endpoint would otherwise have them collide. */
#define INTRING_ADDR    (XDMA_BASE + 0x21000)
/* Per-slot interrupt buffer. Sharing the general transfer buffer would have a
   keyboard and a mouse - and any control transfer between their polls - all
   writing into the same bytes. */
#define INTBUF_ADDR     (XDMA_BASE + 0x22000)
#define XDMA_END        (XDMA_BASE + 0x23000)

#define MAX_SLOTS_USED  8
#define CMD_RING_TRBS   16
#define EVT_RING_TRBS   16
#define EP0_RING_TRBS   16
#define XFER_MAX        4096

/* BAR 0 holds four register files, each reached through its own window: the
   capability registers at 0, the operational ones after them (CAPLENGTH), and
   the runtime and doorbell ones wherever RTSOFF and DBOFF say. Only the first
   XHCI_MAP_SIZE bytes are mapped, so a controller that puts one further out is
   refused rather than reached through an unmapped address. */
#define XHCI_MAP_SIZE  0x10000u
#define XHCI_CAP_SIZE  0x20u                            /* CAPLENGTH..HCCPARAMS2 */
#define XHCI_RT_SIZE   0x40u                            /* MFINDEX, interrupter 0 */
#define XHCI_DB_SIZE   ((MAX_SLOTS_USED + 1) * 4u)      /* command ring + slots */
static struct iowin cap_regs, op_regs, rt_regs, db_regs;
static int present = 0;
static unsigned nports = 0;
static unsigned ctx_size = 32;

static volatile uint64_t *dcbaa;
static struct xhci_trb *cmd_ring, *evt_ring;
static uint8_t *xfer_buf;

static unsigned cmd_enq = 0;      /* our command-ring enqueue index          */
static unsigned cmd_cycle = 1;    /* producer cycle state                    */
static unsigned evt_deq = 0;      /* event-ring dequeue index                */
static unsigned evt_cycle = 1;    /* consumer cycle state                    */

/* per-slot EP0 ring bookkeeping */
static unsigned ep0_enq[MAX_SLOTS_USED + 1];
static unsigned ep0_cycle[MAX_SLOTS_USED + 1];

/* per-slot bulk rings, one each way. Indexed [slot][0]=OUT, [slot][1]=IN.
   `configured` records whether Configure Endpoint has run for that direction -
   xHCI will not accept a transfer on an endpoint the slot does not know about. */
static unsigned bulk_enq[MAX_SLOTS_USED + 1][2];
static unsigned bulk_cycle[MAX_SLOTS_USED + 1][2];
static uint8_t  bulk_configured[MAX_SLOTS_USED + 1][2];
static uint8_t  max_dci[MAX_SLOTS_USED + 1];      /* highest DCI configured, for Context Entries */

static unsigned int_enq[MAX_SLOTS_USED + 1];
static unsigned int_cycle[MAX_SLOTS_USED + 1];
static uint8_t  int_configured[MAX_SLOTS_USED + 1];
static uint8_t  int_pending[MAX_SLOTS_USED + 1];
static uint32_t int_len[MAX_SLOTS_USED + 1];
/* Completion landing pad, per slot. The event ring is SHARED across every
   device on the controller, so a poll that simply took the next event would
   consume another device's completion and report it as its own - which is
   exactly what made a keyboard and a mouse work only one at a time. Events are
   drained and filed by the slot id the TRB carries. */
static uint8_t  int_done[MAX_SLOTS_USED + 1];
static uint32_t int_done_status[MAX_SLOTS_USED + 1];

static inline uint32_t cap_rd(uint32_t o) { return iowin_read32(&cap_regs, o); }
static inline uint32_t op_rd(uint32_t o)  { return iowin_read32(&op_regs, o); }
static inline void op_wr(uint32_t o, uint32_t v) { iowin_write32(&op_regs, o, v); }
static inline void op_wr64(uint32_t o, uint64_t v) {
    /* Split deliberately: several controllers mis-handle a 64-bit MMIO write,
       and the low dword must land first so the high dword completes the value. */
    iowin_write32(&op_regs, o, (uint32_t)v);
    iowin_write32(&op_regs, o + 4, (uint32_t)(v >> 32));
}
static inline uint32_t rt_rd(uint32_t o) { return iowin_read32(&rt_regs, o); }
static inline void rt_wr(uint32_t o, uint32_t v) { iowin_write32(&rt_regs, o, v); }
static inline void rt_wr64(uint32_t o, uint64_t v) {
    iowin_write32(&rt_regs, o, (uint32_t)v);
    iowin_write32(&rt_regs, o + 4, (uint32_t)(v >> 32));
}
static inline void doorbell(unsigned slot, uint32_t v) {
    iowin_write32(&db_regs, slot * 4, v);
}

static inline uint32_t portsc_off(unsigned port1) {
    return XOP_PORTSC_BASE + (port1 - 1) * 0x10;
}
static inline uint32_t portsc_rd(unsigned port1) { return op_rd(portsc_off(port1)); }
static inline void portsc_wr(unsigned port1, uint32_t set) {
    op_wr(portsc_off(port1), (portsc_rd(port1) & XPORT_PRESERVE) | set);
}

static void trb_clear(struct xhci_trb *t) { t->parameter = 0; t->status = 0; t->control = 0; }

/* ---- event ring ---------------------------------------------------------
 * The consumer cycle bit is what distinguishes a fresh event from a stale one
 * left over from the previous pass around the ring; it flips every wrap.
 *
 * The ring is shared by every device and the command ring, so a waiter names
 * the event it wants, and anything else is not taken for it: a keyboard's
 * report completing during a bulk read, or the late completion of a transfer
 * a timeout or a panic abandoned (xhci_reset_ep), would otherwise be read as
 * the answer to whatever was asked next. Fields left 0 match anything. */
struct ev_want {
    uint8_t  type;
    uint8_t  slot;      /* transfer events: the slot ...                     */
    uint8_t  dci;       /* ... and endpoint they must come from              */
    uint32_t trb;       /* the TRB the event must point at                   */
};

/* A transfer completion nobody is waiting for, filed under its slot for
   xhci_interrupt_poll. */
static void file_transfer(const struct xhci_trb *ev) {
    unsigned es = TRB_EV_SLOT(ev->control);
    if (es >= 1 && es <= MAX_SLOTS_USED) {
        int_done[es] = 1;
        int_done_status[es] = ev->status;
    }
}

static int event_matches(const struct xhci_trb *ev, const struct ev_want *w) {
    if ((uint8_t)TRB_GET_TYPE(ev->control) != w->type) return 0;
    if (w->slot && TRB_EV_SLOT(ev->control) != w->slot) return 0;
    if (w->dci && TRB_EV_DCI(ev->control) != w->dci) return 0;
    if (w->trb && (uint32_t)ev->parameter != w->trb) return 0;
    return 1;
}

static int wait_event(const struct ev_want *want, struct xhci_trb *out, int timeout_ms) {
    for (int ms = 0; ms < timeout_ms; ms++) {
        struct xhci_trb *e = &evt_ring[evt_deq];
        uint32_t ctrl = e->control;
        if ((ctrl & TRB_CYCLE) == evt_cycle) {
            struct xhci_trb copy;
            copy.parameter = e->parameter;
            copy.status = e->status;
            copy.control = ctrl;

            if (++evt_deq == EVT_RING_TRBS) { evt_deq = 0; evt_cycle ^= 1; }
            /* Tell the controller how far we have consumed. Bit 3 is EHB
               (event handler busy) and is write-1-to-clear. */
            rt_wr64(0x20 + 0x18, (EVTRING_ADDR + evt_deq * sizeof(struct xhci_trb)) | (1u << 3));

            if (event_matches(&copy, want)) { if (out) *out = copy; return 1; }
            /* Port status change events arrive unsolicited while we work; they
               are consumed above and otherwise ignored. */
            if ((uint8_t)TRB_GET_TYPE(copy.control) == TRB_EV_TRANSFER) file_transfer(&copy);
            continue;
        }
        timer_sleep(1);
    }
    return 0;
}

/* Check the event ring once, without sleeping. Same consumer-cycle logic as
   wait_event, but returns immediately if nothing new has landed - which is what
   an interrupt-endpoint poll needs. */
static int poll_event(uint8_t want_type, struct xhci_trb *out) {
    struct xhci_trb *e = &evt_ring[evt_deq];
    uint32_t ctrl = e->control;
    if ((ctrl & TRB_CYCLE) != evt_cycle) return 0;

    struct xhci_trb copy;
    copy.parameter = e->parameter;
    copy.status = e->status;
    copy.control = ctrl;

    if (++evt_deq == EVT_RING_TRBS) { evt_deq = 0; evt_cycle ^= 1; }
    rt_wr64(0x20 + 0x18, (EVTRING_ADDR + evt_deq * sizeof(struct xhci_trb)) | (1u << 3));

    if ((uint8_t)TRB_GET_TYPE(copy.control) != want_type) return 0;
    if (out) *out = copy;
    return 1;
}

/* Post a command and wait for its completion event. Returns the completion
   code, or 0 on timeout (0 is "Invalid", never a real success). */
static uint8_t run_command(uint64_t param, uint32_t status, uint32_t control,
                           struct xhci_trb *out) {
    uint32_t trb_addr = CMDRING_ADDR + cmd_enq * (uint32_t)sizeof(struct xhci_trb);
    struct xhci_trb *t = &cmd_ring[cmd_enq];
    t->parameter = param;
    t->status = status;
    /* The cycle bit is written LAST as part of control: it is what hands the
       TRB to the controller, so everything else must already be visible. */
    t->control = control | (cmd_cycle ? TRB_CYCLE : 0);

    if (++cmd_enq == CMD_RING_TRBS - 1) {
        /* The final TRB is a Link back to the start; toggling the cycle there
           is what makes the ring circular rather than a buffer we overrun. */
        struct xhci_trb *link = &cmd_ring[CMD_RING_TRBS - 1];
        link->parameter = CMDRING_ADDR;
        link->status = 0;
        link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (cmd_cycle ? TRB_CYCLE : 0);
        cmd_enq = 0;
        cmd_cycle ^= 1;
    }

    doorbell(0, 0);         /* doorbell 0 is the command ring */

    struct xhci_trb ev;
    struct ev_want w = { TRB_EV_CMD_COMPLETE, 0, 0, trb_addr };
    if (!wait_event(&w, &ev, 1000)) return 0;
    if (out) *out = ev;
    return (uint8_t)TRB_CC(ev.status);
}

/* ---- contexts -----------------------------------------------------------
 * Context entries are 32 or 64 bytes depending on HCCPARAMS1.CSZ, which is why
 * every context access is computed rather than indexed as an array. Assuming 32
 * on a 64-byte controller puts the endpoint context at the wrong offset and the
 * Address Device command fails with a parameter error. */
static volatile uint32_t *ctx_at(uint32_t base_phys, unsigned index) {
    return (volatile uint32_t *)(XDMA_V(base_phys) + index * ctx_size);
}

static uint32_t devctx_addr(unsigned slot) { return DEVCTX_ADDR + (slot - 1) * 0x1000; }
static uint32_t ep0ring_addr(unsigned slot) { return EP0RING_ADDR + (slot - 1) * 256; }

static uint32_t speed_to_mps0(enum usb_speed s) {
    switch (s) {
        case USB_SPEED_LOW:   return 8;
        case USB_SPEED_FULL:  return 8;    /* then corrected from the descriptor */
        case USB_SPEED_HIGH:  return 64;
        case USB_SPEED_SUPER: return 512;
        default:              return 8;
    }
}

static uint32_t xspeed_to_id(enum usb_speed s) {
    switch (s) {
        case USB_SPEED_FULL:  return XSPEED_FULL;
        case USB_SPEED_LOW:   return XSPEED_LOW;
        case USB_SPEED_HIGH:  return XSPEED_HIGH;
        case USB_SPEED_SUPER: return XSPEED_SUPER;
        default:              return 0;
    }
}

/* Build the input context for Address Device / Evaluate Context: the control
   context says which pieces are valid, then the slot and EP0 contexts. */
static void build_input_context(struct usb_device *dev, uint32_t mps0, int for_eval) {
    unsigned slot = dev->slot;

    /* zero the whole input context - stale fields are interpreted, not ignored */
    volatile uint8_t *raw = (volatile uint8_t *)XDMA_V(INPUTCTX_ADDR);
    for (unsigned i = 0; i < ctx_size * 3; i++) raw[i] = 0;

    volatile uint32_t *icc = ctx_at(INPUTCTX_ADDR, 0);   /* input control    */
    volatile uint32_t *sc  = ctx_at(INPUTCTX_ADDR, 1);   /* slot context     */
    volatile uint32_t *ep0 = ctx_at(INPUTCTX_ADDR, 2);   /* EP0 context      */

    /* Add flags: bit0 = slot context, bit1 = EP0. Evaluate Context only needs
       EP0 (we are correcting the packet size), Address Device needs both. */
    icc[1] = for_eval ? (1u << 1) : ((1u << 0) | (1u << 1));

    /* slot: route string 0 (root port, no hubs), speed, one context entry */
    sc[0] = (xspeed_to_id(dev->speed) << 20) | (1u << 27);
    sc[1] = ((uint32_t)dev->port << 16);

    /* EP0: control endpoint, three retries, the transfer ring with its cycle */
    ep0[1] = (4u << 3) | (3u << 1) | (mps0 << 16);
    uint32_t ring = ep0ring_addr(slot);
    ep0[2] = ring | 1u;          /* dequeue pointer | DCS */
    ep0[3] = 0;
    ep0[4] = 8;                  /* average TRB length */
}

/* ---- control transfers -------------------------------------------------- */
static int xhci_control(struct usb_device *dev, uint8_t bm_request_type,
                        uint8_t b_request, uint16_t w_value, uint16_t w_index,
                        void *data, uint16_t w_length) {
    if (!present || !dev || dev->slot == 0) return -1;
    if (w_length > XFER_MAX) return -1;

    unsigned slot = dev->slot;
    struct xhci_trb *ring = (struct xhci_trb *)XDMA_V(ep0ring_addr(slot));
    int dev_to_host = (bm_request_type & 0x80) != 0;

    if (!dev_to_host && data && w_length)
        for (uint16_t i = 0; i < w_length; i++) xfer_buf[i] = ((const uint8_t *)data)[i];

    unsigned i = ep0_enq[slot];
    unsigned cyc = ep0_cycle[slot];

    /* Setup Stage. The eight setup bytes travel INSIDE the TRB (IDT), not via a
       pointer - which is why parameter is built by hand here. */
    struct usb_setup sp;
    sp.bm_request_type = bm_request_type;
    sp.b_request = b_request;
    sp.w_value = w_value;
    sp.w_index = w_index;
    sp.w_length = w_length;
    uint64_t setup_data = 0;
    for (int b = 0; b < 8; b++)
        setup_data |= (uint64_t)((const uint8_t *)&sp)[b] << (b * 8);

    uint32_t trt = w_length ? (dev_to_host ? 3u : 2u) : 0u;   /* transfer type */
    struct xhci_trb *t = &ring[i];
    t->parameter = setup_data;
    t->status = 8;
    t->control = TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT | (trt << 16) | (cyc ? TRB_CYCLE : 0);
    if (++i == EP0_RING_TRBS - 1) { i = 0; cyc ^= 1; }

    if (w_length) {
        t = &ring[i];
        t->parameter = XFERBUF_ADDR;
        t->status = w_length;
        t->control = TRB_TYPE(TRB_DATA_STAGE) | (dev_to_host ? (1u << 16) : 0)
                   | TRB_ISP | (cyc ? TRB_CYCLE : 0);
        if (++i == EP0_RING_TRBS - 1) { i = 0; cyc ^= 1; }
    }

    /* Status Stage runs opposite to the data, and carries IOC so we get an event. */
    t = &ring[i];
    t->parameter = 0;
    t->status = 0;
    t->control = TRB_TYPE(TRB_STATUS_STAGE) | (dev_to_host ? 0 : (1u << 16))
               | TRB_IOC | (cyc ? TRB_CYCLE : 0);
    if (++i == EP0_RING_TRBS - 1) {
        struct xhci_trb *link = &ring[EP0_RING_TRBS - 1];
        link->parameter = ep0ring_addr(slot);
        link->status = 0;
        link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (cyc ? TRB_CYCLE : 0);
        i = 0; cyc ^= 1;
    }
    ep0_enq[slot] = i;
    ep0_cycle[slot] = cyc;

    doorbell(slot, 1);          /* DCI 1 = EP0 */

    struct xhci_trb ev;
    struct ev_want w = { TRB_EV_TRANSFER, (uint8_t)slot, 1, 0 };
    if (!wait_event(&w, &ev, 1000)) return -1;
    uint8_t cc = (uint8_t)TRB_CC(ev.status);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) return -1;

    /* On a transfer event the low 24 bits of status are the RESIDUAL - what was
       not transferred - so the actual count is the request minus that. */
    int residual = (int)(ev.status & 0x00FFFFFF);
    int transferred = (int)w_length - residual;
    if (transferred < 0) transferred = 0;
    if (transferred > (int)w_length) transferred = (int)w_length;

    if (dev_to_host && data && transferred > 0)
        for (int b = 0; b < transferred; b++) ((uint8_t *)data)[b] = xfer_buf[b];

    return transferred;
}

/* ---- attach: enable a slot and let the controller address the device ----- */
static int xhci_attach(struct usb_device *dev) {
    struct xhci_trb ev;

    uint8_t cc = run_command(0, 0, TRB_TYPE(TRB_CMD_ENABLE_SLOT), &ev);
    if (cc != TRB_CC_SUCCESS) {
        klog_u32("xHCI", SEV_WARN, "Enable Slot failed, completion code ", cc,
                 LOG_COLOR_VALUE, "");
        return 0;
    }
    unsigned slot = (ev.control >> 24) & 0xFF;
    if (slot == 0 || slot > MAX_SLOTS_USED) {
        klog_u32("xHCI", SEV_WARN, "controller returned an unusable slot id ", slot,
                 LOG_COLOR_VALUE, "");
        return 0;
    }
    dev->slot = slot;

    /* Device context: the controller writes here, we only provide it. */
    volatile uint8_t *dc = (volatile uint8_t *)XDMA_V(devctx_addr(slot));
    for (unsigned i = 0; i < ctx_size * 32; i++) dc[i] = 0;
    dcbaa[slot] = devctx_addr(slot);

    /* EP0 transfer ring */
    struct xhci_trb *ring = (struct xhci_trb *)XDMA_V(ep0ring_addr(slot));
    for (unsigned i = 0; i < EP0_RING_TRBS; i++) trb_clear(&ring[i]);
    ep0_enq[slot] = 0;
    ep0_cycle[slot] = 1;
    bulk_configured[slot][0] = bulk_configured[slot][1] = 0;
    max_dci[slot] = 1;
    int_configured[slot] = 0;
    int_pending[slot] = 0;
    int_done[slot] = 0;

    uint32_t mps0 = speed_to_mps0(dev->speed);
    build_input_context(dev, mps0, 0);

    cc = run_command(INPUTCTX_ADDR, 0,
                     TRB_TYPE(TRB_CMD_ADDRESS_DEV) | ((uint32_t)slot << 24), &ev);
    if (cc != TRB_CC_SUCCESS) {
        klog_u32("xHCI", SEV_WARN, "Address Device failed, completion code ", cc,
                 LOG_COLOR_VALUE, "");
        return 0;
    }

    /* The controller chose the address and wrote it into the output slot
       context; read it back rather than assuming. */
    volatile uint32_t *out_slot = ctx_at(devctx_addr(slot), 0);
    dev->address = (uint8_t)(out_slot[3] & 0xFF);
    dev->max_packet0 = (uint8_t)mps0;

    /* Full and low speed devices report their real bMaxPacketSize0 only in the
       descriptor, and the guess above is frequently wrong for them. Read it and
       correct the endpoint context if it differs - a mismatched packet size
       makes later transfers fail rather than run slowly. */
    struct usb_device_descriptor dd;
    for (unsigned i = 0; i < sizeof(dd); i++) ((uint8_t *)&dd)[i] = 0;
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &dd, 8) >= 8 &&
        dd.b_max_packet_size0 && dd.b_max_packet_size0 != (uint8_t)mps0) {
        build_input_context(dev, dd.b_max_packet_size0, 1);
        run_command(INPUTCTX_ADDR, 0,
                    TRB_TYPE(TRB_CMD_EVAL_CTX) | ((uint32_t)slot << 24), &ev);
        dev->max_packet0 = dd.b_max_packet_size0;
    }
    return 1;
}

/* ---- bulk transfers -----------------------------------------------------
 * An endpoint has to exist in the slot's device context before the controller
 * will carry anything on it, which is what Configure Endpoint does. That is a
 * command, not a register write, so it happens lazily on first use rather than
 * during enumeration - the core has issued SET_CONFIGURATION by then and the
 * endpoint descriptors are known.
 *
 * The Device Context Index is the endpoint's slot in the context array:
 * endpoint N maps to 2N for OUT and 2N+1 for IN, with DCI 1 reserved for the
 * bidirectional EP0. Getting this mapping wrong addresses somebody else's
 * endpoint. */
static unsigned dci_for(uint8_t ep) {
    unsigned n = ep & 0x0F;
    return n * 2 + ((ep & 0x80) ? 1 : 0);
}

static uint32_t bulkring_addr(unsigned slot, int in) {
    return BULKRING_ADDR + ((slot - 1) * 2 + (in ? 1 : 0)) * 256;
}

#define BULK_RING_TRBS 16

/* ep_type is the xHCI endpoint type: 2 = Bulk OUT, 6 = Bulk IN, 3 = Interrupt
   OUT, 7 = Interrupt IN. `interval` is the encoded polling interval, ignored
   for bulk. `redo` drops the endpoint and adds it back in the one command,
   which is how an endpoint that already exists is put back to its first
   state - toggle and all (xhci_reset_ep). */
static int configure_ep(struct usb_device *dev, uint8_t ep, uint16_t mps,
                        uint32_t ring, unsigned ep_type, uint32_t interval, int redo) {
    unsigned slot = dev->slot;
    unsigned dci = dci_for(ep);
    if (dci > max_dci[slot]) max_dci[slot] = (uint8_t)dci;

    /* Input context: the control context names what is being added, then the
       slot context (whose "context entries" must reach the highest DCI in
       use, not just this one, or a later endpoint would cut off an earlier
       one) and the endpoint context itself. */
    volatile uint8_t *raw = (volatile uint8_t *)XDMA_V(INPUTCTX_ADDR);
    for (unsigned i = 0; i < ctx_size * 32; i++) raw[i] = 0;

    volatile uint32_t *icc = ctx_at(INPUTCTX_ADDR, 0);
    volatile uint32_t *sc  = ctx_at(INPUTCTX_ADDR, 1);
    volatile uint32_t *epc = ctx_at(INPUTCTX_ADDR, dci + 1);

    if (redo) icc[0] = 1u << dci;           /* drop it ... */
    icc[1] = (1u << 0) | (1u << dci);       /* ... and add: slot + this endpoint */
    sc[0] = (xspeed_to_id(dev->speed) << 20) | ((uint32_t)max_dci[slot] << 27);
    sc[1] = ((uint32_t)dev->port << 16);

    epc[0] = interval << 16;
    epc[1] = ((uint32_t)ep_type << 3) | (3u << 1) | ((uint32_t)mps << 16);
    epc[2] = ring | 1u;                     /* dequeue pointer | DCS */
    epc[3] = 0;
    epc[4] = mps;                           /* average TRB length */

    struct xhci_trb ev;
    uint8_t cc = run_command(INPUTCTX_ADDR, 0,
                             TRB_TYPE(TRB_CMD_CONFIG_EP) | ((uint32_t)slot << 24), &ev);
    if (cc != TRB_CC_SUCCESS) {
        klog_u32("xHCI", SEV_WARN, "Configure Endpoint failed, completion code ", cc,
                 LOG_COLOR_VALUE, "");
        return 0;
    }
    return 1;
}

static int xhci_bulk(struct usb_device *dev, uint8_t ep, void *data, uint32_t len) {
    if (!present || !dev || dev->slot == 0 || !data) return -1;
    if (len == 0 || len > XFER_MAX) return -1;

    unsigned slot = dev->slot;
    int in = (ep & 0x80) != 0;
    uint16_t mps = in ? dev->ep_in_mps : dev->ep_out_mps;
    if (mps == 0) mps = 512;

    if (!bulk_configured[slot][in]) {
        struct xhci_trb *r = (struct xhci_trb *)XDMA_V(bulkring_addr(slot, in));
        for (unsigned i = 0; i < BULK_RING_TRBS; i++) trb_clear(&r[i]);
        bulk_enq[slot][in] = 0;
        bulk_cycle[slot][in] = 1;
        if (!configure_ep(dev, ep, mps, bulkring_addr(slot, in), in ? 6u : 2u, 0, 0))
            return -1;
        bulk_configured[slot][in] = 1;
    }

    if (!in) for (uint32_t i = 0; i < len; i++) xfer_buf[i] = ((const uint8_t *)data)[i];

    struct xhci_trb *ring = (struct xhci_trb *)XDMA_V(bulkring_addr(slot, in));
    unsigned i = bulk_enq[slot][in];
    unsigned cyc = bulk_cycle[slot][in];
    uint32_t trb_addr = bulkring_addr(slot, in) + i * (uint32_t)sizeof(struct xhci_trb);

    struct xhci_trb *t = &ring[i];
    t->parameter = XFERBUF_ADDR;
    t->status = len;
    /* ISP so a short packet completes rather than being an error - a CSW read
       is routinely shorter than the buffer offered for it. */
    t->control = TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP | (cyc ? TRB_CYCLE : 0);

    if (++i == BULK_RING_TRBS - 1) {
        struct xhci_trb *link = &ring[BULK_RING_TRBS - 1];
        link->parameter = bulkring_addr(slot, in);
        link->status = 0;
        link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (cyc ? TRB_CYCLE : 0);
        i = 0; cyc ^= 1;
    }
    bulk_enq[slot][in] = i;
    bulk_cycle[slot][in] = cyc;

    doorbell(slot, dci_for(ep));

    struct xhci_trb ev;
    struct ev_want w = { TRB_EV_TRANSFER, (uint8_t)slot, (uint8_t)dci_for(ep), trb_addr };
    if (!wait_event(&w, &ev, 2000)) return -1;
    uint8_t cc = (uint8_t)TRB_CC(ev.status);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) return -1;

    int residual = (int)(ev.status & 0x00FFFFFF);
    int transferred = (int)len - residual;
    if (transferred < 0) transferred = 0;
    if (transferred > (int)len) transferred = (int)len;

    if (in && transferred > 0)
        for (int b = 0; b < transferred; b++) ((uint8_t *)data)[b] = xfer_buf[b];
    return transferred;
}

static uint32_t intring_addr(unsigned slot) { return INTRING_ADDR + (slot - 1) * 256; }
static uint32_t intbuf_addr(unsigned slot)  { return INTBUF_ADDR  + (slot - 1) * 256; }

static int xhci_interrupt_poll(struct usb_device *dev, uint8_t ep,
                               void *data, uint32_t len) {
    if (!present || !dev || dev->slot == 0 || !data) return -1;
    if (len == 0 || len > 64) return -1;

    unsigned slot = dev->slot;
    uint16_t mps = dev->ep_int_mps ? dev->ep_int_mps : 8;

    if (!int_configured[slot]) {
        struct xhci_trb *r = (struct xhci_trb *)XDMA_V(intring_addr(slot));
        for (unsigned i = 0; i < BULK_RING_TRBS; i++) trb_clear(&r[i]);
        int_enq[slot] = 0;
        int_cycle[slot] = 1;
        /* bInterval for a high-speed interrupt endpoint is already the
           2^(n-1) microframe exponent xHCI wants; a full-speed one counts
           frames, and 3 (8 microframes = 1 ms) is the usual mapping. */
        uint32_t interval = dev->speed == USB_SPEED_HIGH
                          ? (dev->ep_int_interval ? dev->ep_int_interval - 1 : 3) : 3;
        if (!configure_ep(dev, ep, mps, intring_addr(slot), 7u, interval, 0)) return -1;
        int_configured[slot] = 1;
        int_pending[slot] = 0;
    }

    if (!int_pending[slot]) {
        struct xhci_trb *ring = (struct xhci_trb *)XDMA_V(intring_addr(slot));
        unsigned i = int_enq[slot], cyc = int_cycle[slot];
        struct xhci_trb *t = &ring[i];
        t->parameter = intbuf_addr(slot);
        t->status = len;
        t->control = TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP | (cyc ? TRB_CYCLE : 0);
        if (++i == BULK_RING_TRBS - 1) {
            struct xhci_trb *link = &ring[BULK_RING_TRBS - 1];
            link->parameter = intring_addr(slot);
            link->status = 0;
            link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (cyc ? TRB_CYCLE : 0);
            i = 0; cyc ^= 1;
        }
        int_enq[slot] = i;
        int_cycle[slot] = cyc;
        int_len[slot] = len;
        doorbell(slot, dci_for(ep));
        int_pending[slot] = 1;
        return 0;
    }

    /* Drain whatever has landed and file each completion under its own slot,
       then look only at ours. */
    struct xhci_trb ev;
    while (poll_event(TRB_EV_TRANSFER, &ev)) file_transfer(&ev);
    if (!int_done[slot]) return 0;                      /* nothing for us yet */
    int_done[slot] = 0;
    int_pending[slot] = 0;

    uint32_t status = int_done_status[slot];
    uint8_t cc = (uint8_t)TRB_CC(status);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) return -1;

    int residual = (int)(status & 0x00FFFFFF);
    int got = (int)int_len[slot] - residual;
    if (got < 0) got = 0;
    if (got > (int)len) got = (int)len;
    const uint8_t *src = (const uint8_t *)XDMA_V(intbuf_addr(slot));
    for (int b = 0; b < got; b++) ((uint8_t *)data)[b] = src[b];
    return got;
}

/* Endpoint state, bits 2:0 of an endpoint context's first dword. */
#define EP_STATE_DISABLED 0
#define EP_STATE_RUNNING  1
#define EP_STATE_HALTED   2

/* Bring an endpoint back to a known state (usb.h). xHCI keeps the endpoint's
   state, data toggle included, in the controller, so this is done by command.
   First the endpoint is stopped where it is: a halted one (after a stall)
   takes Reset Endpoint, a running one - perhaps with a transfer still on its
   ring, which is what a panic or a timeout leaves - takes Stop Endpoint.
   Then a bulk endpoint is dropped and added back on a fresh ring, as Linux's
   xhci_endpoint_reset does: neither command above puts the toggle back at
   DATA0, which a device whose stall was just cleared expects. The control
   endpoint has no toggle to keep in step, so Set TR Dequeue Pointer moving
   the controller past what was abandoned is enough there. Late completions
   of abandoned TRBs are not mistaken for new ones (struct ev_want). */
static void xhci_reset_ep(struct usb_device *dev, uint8_t ep) {
    if (!present || !dev || dev->slot == 0) return;
    unsigned slot = dev->slot;
    int bulk = (ep & 0x0F) != 0;
    int in = (ep & 0x80) != 0;
    if (bulk && !bulk_configured[slot][in]) return;      /* nothing ever ran on it */
    unsigned dci = bulk ? dci_for(ep) : 1;

    /* The OUTPUT device context: the slot context, then one per DCI. */
    volatile uint32_t *epc = ctx_at(devctx_addr(slot), dci);
    uint32_t state = epc[0] & 7u;
    if (state == EP_STATE_DISABLED) return;
    uint32_t id = ((uint32_t)slot << 24) | ((uint32_t)dci << 16);
    if (state == EP_STATE_HALTED)
        run_command(0, 0, TRB_TYPE(TRB_CMD_RESET_EP) | id, 0);
    else if (state == EP_STATE_RUNNING)
        run_command(0, 0, TRB_TYPE(TRB_CMD_STOP_EP) | id, 0);

    if (!bulk) {
        uint32_t deq = ep0ring_addr(slot) + ep0_enq[slot] * (uint32_t)sizeof(struct xhci_trb);
        run_command(deq | (ep0_cycle[slot] ? 1u : 0u), 0, TRB_TYPE(TRB_CMD_SET_TR_DEQ) | id, 0);
        return;
    }
    struct xhci_trb *r = (struct xhci_trb *)XDMA_V(bulkring_addr(slot, in));
    for (unsigned i = 0; i < BULK_RING_TRBS; i++) trb_clear(&r[i]);
    bulk_enq[slot][in] = 0;
    bulk_cycle[slot][in] = 1;
    uint16_t mps = in ? dev->ep_in_mps : dev->ep_out_mps;
    if (mps == 0) mps = 512;
    if (!configure_ep(dev, ep, mps, bulkring_addr(slot, in), in ? 6u : 2u, 0, 1))
        bulk_configured[slot][in] = 0;   /* the next transfer configures it afresh */
}

static const struct usb_hc_ops xhci_ops = {
    .name    = "xHCI",
    .attach  = xhci_attach,
    .control = xhci_control,
    .bulk    = xhci_bulk,
    .interrupt_poll = xhci_interrupt_poll,
    .reset_ep = xhci_reset_ep,
};

/* ---- ports -------------------------------------------------------------- */
static enum usb_speed port_speed(uint32_t sc) {
    switch (XPORT_SPEED(sc)) {
        case XSPEED_FULL:  return USB_SPEED_FULL;
        case XSPEED_LOW:   return USB_SPEED_LOW;
        case XSPEED_HIGH:  return USB_SPEED_HIGH;
        case XSPEED_SUPER: return USB_SPEED_SUPER;
        default:           return USB_SPEED_UNKNOWN;
    }
}

static void enumerate_port(unsigned port1) {
    uint32_t sc = portsc_rd(port1);
    if (!(sc & XPORT_CCS)) return;

    /* A USB 3 port reports itself enabled straight out of reset; a USB 2 port
       needs the reset driven explicitly. Doing the reset either way is
       harmless and covers both. */
    if (!(sc & XPORT_PED)) {
        portsc_wr(port1, XPORT_PR);
        int ok = 0;
        for (int i = 0; i < 100; i++) {
            timer_sleep(1);
            sc = portsc_rd(port1);
            if (sc & XPORT_PRC) { ok = 1; break; }
        }
        /* clear the reset-change flag (write 1) */
        op_wr(portsc_off(port1), (sc & XPORT_PRESERVE) | XPORT_PRC);
        if (!ok) {
            klog_u32("xHCI", SEV_WARN, "port ", port1, LOG_COLOR_VALUE,
                     ": reset never completed");
            return;
        }
        sc = portsc_rd(port1);
    }
    if (!(sc & XPORT_PED)) return;       /* nothing usable here */

    enum usb_speed sp = port_speed(sc);
    if (sp == USB_SPEED_UNKNOWN) {
        klog_u32("xHCI", SEV_WARN, "port ", port1, LOG_COLOR_VALUE,
                 ": controller reported an unknown speed id");
        return;
    }

    struct usb_device *dev = usb_alloc_device(&xhci_ops, 0, (uint8_t)port1, sp);
    if (!dev) { klog("xHCI", SEV_WARN, "device table full"); return; }
    usb_enumerate(dev);
}

/* ---- MSI delivery self-test ---------------------------------------------
 * Configuring MSI proves nothing - an interrupt arriving does. xHCI is the
 * right device to prove it on: it has a real MSI-X capability, and a No-Op
 * command gives a way to make it raise an interrupt on demand.
 *
 * The whole path gets exercised: the controller writes a dword to 0xFEE.....,
 * the local APIC turns that into a vector, the IDT dispatches it, and the
 * handler runs. The driver goes straight back to polling afterwards, so a
 * failure here costs nothing. */
static volatile uint32_t msi_hits = 0;

static void xhci_msi_handler(struct registers *r) {
    (void)r;
    msi_hits++;
    /* Acknowledge at the device as well as the APIC: the interrupter's pending
       bit is write-1-to-clear and a set one suppresses the next interrupt. */
    rt_wr(0x20 + 0x00, rt_rd(0x20 + 0x00) | 1u);
}

static void msi_self_test(const struct pci_device *dev) {
    if (!irq_msi_available()) return;

    int vector = irq_alloc_msi_vector(xhci_msi_handler);
    if (vector < 0) return;

    if (!pci_msi_setup(dev, (uint8_t)vector)) {
        irq_free_msi_vector(vector);
        klog("xHCI", SEV_INFO, "no MSI or MSI-X capability on this controller");
        return;
    }

    msi_hits = 0;
    rt_wr(0x20 + 0x00, 0x3);                     /* interrupter 0: pending clear + enable */
    op_wr(XOP_USBCMD, op_rd(XOP_USBCMD) | (1u << 2));   /* INTE */

    struct xhci_trb ev;
    run_command(0, 0, TRB_TYPE(TRB_CMD_NOOP) | TRB_IOC, &ev);

    for (int i = 0; i < 200000 && msi_hits == 0; i++) { }

    uint32_t hits = msi_hits;
    op_wr(XOP_USBCMD, op_rd(XOP_USBCMD) & ~(1u << 2));  /* back to polling */
    rt_wr(0x20 + 0x00, 0x1);
    pci_msi_disable(dev);
    irq_free_msi_vector(vector);

    if (hits) klog_u32("xHCI", SEV_OK, "MSI delivered on vector ",
                       (uint32_t)vector, LOG_COLOR_VALUE, "");
    else      klog_u32("xHCI", SEV_WARN, "MSI configured but never delivered, vector ",
                       (uint32_t)vector, LOG_COLOR_VALUE, "");
}

int xhci_init(void) {
    present = 0;

    /* class 0x0C serial bus, subclass 0x03 USB, prog-if 0x30 = xHCI */
    const struct pci_device *dev = pci_find(0x0C, 0x03, 0x30, 0);
    if (!dev) return 0;

    pci_enable_bus_master(dev);

    uint32_t base = pci_bar_mmio32(dev, 0, "xHCI");
    if (base == 0) return 0;

    /* xHCI register files are large - runtime and doorbell regions sit well
       past the operational registers - so map generously. */
    for (uint32_t off = 0; off < XHCI_MAP_SIZE; off += 0x1000)
        paging_map_kernel(base + off, base + off, VM_WRITE | VM_UNCACHED);
    if (iowin_mmio(&cap_regs, (volatile void *)base, XHCI_CAP_SIZE) != 0) return 0;

    uint32_t hcs1 = cap_rd(XCAP_HCSPARAMS1);
    uint32_t caplen = iowin_read8(&cap_regs, XCAP_CAPLENGTH);
    uint32_t op_size = XOP_PORTSC_BASE + HCS1_MAXPORTS(hcs1) * 0x10u;
    uint32_t rtsoff = cap_rd(XCAP_RTSOFF) & ~0x1Fu;
    uint32_t dboff  = cap_rd(XCAP_DBOFF) & ~0x3u;
    if (caplen < XHCI_CAP_SIZE || caplen + op_size > XHCI_MAP_SIZE ||
        rtsoff > XHCI_MAP_SIZE - XHCI_RT_SIZE || dboff > XHCI_MAP_SIZE - XHCI_DB_SIZE ||
        iowin_mmio(&op_regs, (volatile void *)(base + caplen), op_size) != 0 ||
        iowin_mmio(&rt_regs, (volatile void *)(base + rtsoff), XHCI_RT_SIZE) != 0 ||
        iowin_mmio(&db_regs, (volatile void *)(base + dboff), XHCI_DB_SIZE) != 0) {
        klog("xHCI", SEV_WARN, "register files outside the mapped BAR - not initialising");
        return 0;
    }

    uint32_t hcs2 = cap_rd(XCAP_HCSPARAMS2);
    uint32_t hcc1 = cap_rd(XCAP_HCCPARAMS1);
    nports = HCS1_MAXPORTS(hcs1);
    unsigned maxslots = HCS1_MAXSLOTS(hcs1);
    ctx_size = HCC1_CSZ(hcc1) ? 64 : 32;
    if (nports == 0 || maxslots == 0) return 0;

    for (uint32_t a = XDMA_BASE; a < XDMA_END; a += 0x1000)
        paging_map_kernel(XDMA_V(a), a, VM_WRITE);

    /* Wait for the controller to finish its own power-on before touching it. */
    for (int i = 0; i < 100 && (op_rd(XOP_USBSTS) & XSTS_CNR); i++) timer_sleep(1);

    op_wr(XOP_USBCMD, op_rd(XOP_USBCMD) & ~XCMD_RS);
    for (int i = 0; i < 100 && !(op_rd(XOP_USBSTS) & XSTS_HCH); i++) timer_sleep(1);

    op_wr(XOP_USBCMD, XCMD_HCRST);
    int ok = 0;
    for (int i = 0; i < 500; i++) {
        if (!(op_rd(XOP_USBCMD) & XCMD_HCRST) && !(op_rd(XOP_USBSTS) & XSTS_CNR)) {
            ok = 1; break;
        }
        timer_sleep(1);
    }
    if (!ok) { klog("xHCI", SEV_WARN, "controller reset did not complete"); return 0; }

    uint32_t spad;
    if (xhci_spad_plan(hcs2, op_rd(XOP_PAGESIZE), &spad) != 0) {
        klog("xHCI", SEV_WARN, "controller cannot use 4 KB pages - not initialising");
        return 0;
    }

    if (maxslots > MAX_SLOTS_USED) maxslots = MAX_SLOTS_USED;
    op_wr(XOP_CONFIG, maxslots);

    /* device context base address array */
    dcbaa = (volatile uint64_t *)XDMA_V(DCBAA_ADDR);
    for (unsigned i = 0; i <= maxslots; i++) dcbaa[i] = 0;

    /* Scratchpad: memory the controller wants for its own use. Entry 0 of the
       DCBAA points at the array of pages. Skipping this on a controller that
       asks for them is a controller that never starts. */
    if (spad) {
        /* Kept across a second xhci_init rather than leaked. */
        static paddr_t spad_array;
        static uint32_t spad_count;
        xhci_spad_release(spad_array, spad_count);
        spad_array = 0; spad_count = 0;
        if (xhci_spad_build(spad, &spad_array) != 0) {
            klog_u32("xHCI", SEV_WARN, "no memory for the scratchpad pages asked for: ",
                     spad, LOG_COLOR_VALUE, "");
            return 0;
        }
        spad_count = spad;
        dcbaa[0] = spad_array;
        klog_u32("xHCI", SEV_INFO, "scratchpad pages: ", spad, LOG_COLOR_VALUE, "");
    }
    op_wr64(XOP_DCBAAP, DCBAA_ADDR);

    /* command ring, with its trailing Link TRB back to the start */
    cmd_ring = (struct xhci_trb *)XDMA_V(CMDRING_ADDR);
    for (unsigned i = 0; i < CMD_RING_TRBS; i++) trb_clear(&cmd_ring[i]);
    cmd_ring[CMD_RING_TRBS - 1].parameter = CMDRING_ADDR;
    cmd_ring[CMD_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;
    cmd_enq = 0; cmd_cycle = 1;
    op_wr64(XOP_CRCR, CMDRING_ADDR | 1u);     /* | RCS */

    /* event ring: one segment, described by a one-entry segment table */
    evt_ring = (struct xhci_trb *)XDMA_V(EVTRING_ADDR);
    for (unsigned i = 0; i < EVT_RING_TRBS; i++) trb_clear(&evt_ring[i]);
    evt_deq = 0; evt_cycle = 1;

    volatile uint32_t *erst = (volatile uint32_t *)XDMA_V(ERST_ADDR);
    erst[0] = EVTRING_ADDR;
    erst[1] = 0;
    erst[2] = EVT_RING_TRBS;
    erst[3] = 0;

    /* Interrupter 0 lives at runtime offset 0x20. ERDP must be set BEFORE
       ERSTBA: writing the segment table base is what arms the interrupter. */
    rt_wr(0x20 + 0x08, 1);                              /* ERSTSZ = 1 segment */
    rt_wr64(0x20 + 0x18, EVTRING_ADDR);                 /* ERDP               */
    rt_wr64(0x20 + 0x10, ERST_ADDR);                    /* ERSTBA             */

    xfer_buf = (uint8_t *)XDMA_V(XFERBUF_ADDR);

    op_wr(XOP_USBCMD, XCMD_RS);
    for (int i = 0; i < 100 && (op_rd(XOP_USBSTS) & XSTS_HCH); i++) timer_sleep(1);
    if (op_rd(XOP_USBSTS) & XSTS_HCH) {
        klog("xHCI", SEV_WARN, "controller would not leave the halted state");
        return 0;
    }

    present = 1;
    klog_u32("xHCI", SEV_OK, "controller online, root ports: ", nports, LOG_COLOR_VALUE, "");

    msi_self_test(dev);

    /* Power every port, then let devices settle before looking. */
    for (unsigned p = 1; p <= nports; p++)
        if (!(portsc_rd(p) & XPORT_PP)) portsc_wr(p, XPORT_PP);
    timer_sleep(100);

    for (unsigned p = 1; p <= nports; p++) enumerate_port(p);
    return 1;
}
