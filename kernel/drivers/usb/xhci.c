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

/* ---- DMA memory ---------------------------------------------------------
 * Each controller has its own pages (usb_dma_alloc): physical for the
 * controller, a kernel-half alias for the CPU so the structures stay reachable
 * from a process address space. The offsets below are within those pages.
 *
 * xHCI's alignment rules are strict and silent when violated: the DCBAA, the
 * command ring, the ERST and every context must be 64-byte aligned. Page
 * granularity below satisfies all of that with room to spare. */
#define XDMA_VBASE      0xE4000000u     /* controller i's alias: + i * 0x20000 */
#define XDMA_PAGES      0x12

#define DCBAA_OFF       0x0000
#define CMDRING_OFF     0x1000
#define EVTRING_OFF     0x2000
#define ERST_OFF        0x3000
#define INPUTCTX_OFF    0x4000
#define DEVCTX_OFF      0x5000          /* one page per slot          */
#define EP0RING_OFF     0xD000          /* 256 bytes per slot         */
#define XFERBUF_OFF     0xE000
#define BULKRING_OFF    0xF000          /* 256 bytes per slot per dir */
/* Its own ring rather than sharing the bulk IN one: a composite device with
   both a bulk and an interrupt IN endpoint would otherwise have them collide. */
#define INTRING_OFF     0x10000
/* Per-slot interrupt buffer. Sharing the general transfer buffer would have a
   keyboard and a mouse - and any control transfer between their polls - all
   writing into the same bytes. */
#define INTBUF_OFF      0x11000
_Static_assert(INTBUF_OFF + 0x1000 == XDMA_PAGES * 0x1000, "xHCI DMA layout overruns its pages");

#define MAX_SLOTS_USED  8
#define CMD_RING_TRBS   16
#define EVT_RING_TRBS   16
#define EP0_RING_TRBS   16
#define XFER_MAX        4096

/* AM3+ boards carry their USB 3 ports on one or two add-on controllers. */
#define XHCI_MAX_HCS    4

/* BAR 0 holds four register files, each reached through its own window: the
   capability registers at 0, the operational ones after them (CAPLENGTH), and
   the runtime and doorbell ones wherever RTSOFF and DBOFF say. Only the first
   XHCI_MAP_SIZE bytes are mapped, so a controller that puts one further out is
   refused rather than reached through an unmapped address. */
#define XHCI_MAP_SIZE  0x10000u
#define XHCI_CAP_SIZE  0x20u                            /* CAPLENGTH..HCCPARAMS2 */
#define XHCI_RT_SIZE   0x40u                            /* MFINDEX, interrupter 0 */
#define XHCI_DB_SIZE   ((MAX_SLOTS_USED + 1) * 4u)      /* command ring + slots */

/* Everything one controller needs; a usb_device's `hc` points at its
   controller's. */
struct xhci_hc {
    struct iowin cap_regs, op_regs, rt_regs, db_regs;
    unsigned nports;
    unsigned ctx_size;

    uint32_t dma;                   /* physical base of the DMA pages */
    uint8_t *dma_v;                 /* the CPU's alias of them        */
    volatile uint64_t *dcbaa;
    struct xhci_trb *cmd_ring, *evt_ring;
    uint8_t *xfer_buf;
    paddr_t spad_array;             /* scratchpad pages, page allocator */
    uint32_t spad_count;

    unsigned cmd_enq;               /* our command-ring enqueue index          */
    unsigned cmd_cycle;             /* producer cycle state                    */
    unsigned evt_deq;               /* event-ring dequeue index                */
    unsigned evt_cycle;             /* consumer cycle state                    */

    /* per-slot EP0 ring bookkeeping */
    unsigned ep0_enq[MAX_SLOTS_USED + 1];
    unsigned ep0_cycle[MAX_SLOTS_USED + 1];

    /* per-slot bulk rings, one each way. Indexed [slot][0]=OUT, [slot][1]=IN.
       `configured` records whether Configure Endpoint has run for that
       direction - xHCI will not accept a transfer on an endpoint the slot does
       not know about. */
    unsigned bulk_enq[MAX_SLOTS_USED + 1][2];
    unsigned bulk_cycle[MAX_SLOTS_USED + 1][2];
    uint8_t  bulk_configured[MAX_SLOTS_USED + 1][2];
    uint8_t  max_dci[MAX_SLOTS_USED + 1];      /* highest DCI configured, for Context Entries */

    unsigned int_enq[MAX_SLOTS_USED + 1];
    unsigned int_cycle[MAX_SLOTS_USED + 1];
    uint8_t  int_configured[MAX_SLOTS_USED + 1];
    uint8_t  int_pending[MAX_SLOTS_USED + 1];
    uint32_t int_len[MAX_SLOTS_USED + 1];
    /* Completion landing pad, per slot. The event ring is SHARED across every
       device on the controller, so a poll that simply took the next event
       would consume another device's completion and report it as its own -
       which is exactly what made a keyboard and a mouse work only one at a
       time. Events are drained and filed by the slot id the TRB carries. */
    uint8_t  int_done[MAX_SLOTS_USED + 1];
    uint32_t int_done_status[MAX_SLOTS_USED + 1];
};

static struct xhci_hc hcs[XHCI_MAX_HCS];
static unsigned nhcs = 0;

static inline uint32_t dma_phys(const struct xhci_hc *c, uint32_t off) { return c->dma + off; }
static inline void    *dma_virt(const struct xhci_hc *c, uint32_t off) { return c->dma_v + off; }

static inline uint32_t cap_rd(struct xhci_hc *c, uint32_t o) { return iowin_read32(&c->cap_regs, o); }
static inline uint32_t op_rd(struct xhci_hc *c, uint32_t o)  { return iowin_read32(&c->op_regs, o); }
static inline void op_wr(struct xhci_hc *c, uint32_t o, uint32_t v) { iowin_write32(&c->op_regs, o, v); }
static inline void op_wr64(struct xhci_hc *c, uint32_t o, uint64_t v) {
    /* Split deliberately: several controllers mis-handle a 64-bit MMIO write,
       and the low dword must land first so the high dword completes the value. */
    iowin_write32(&c->op_regs, o, (uint32_t)v);
    iowin_write32(&c->op_regs, o + 4, (uint32_t)(v >> 32));
}
static inline uint32_t rt_rd(struct xhci_hc *c, uint32_t o) { return iowin_read32(&c->rt_regs, o); }
static inline void rt_wr(struct xhci_hc *c, uint32_t o, uint32_t v) { iowin_write32(&c->rt_regs, o, v); }
static inline void rt_wr64(struct xhci_hc *c, uint32_t o, uint64_t v) {
    iowin_write32(&c->rt_regs, o, (uint32_t)v);
    iowin_write32(&c->rt_regs, o + 4, (uint32_t)(v >> 32));
}
static inline void doorbell(struct xhci_hc *c, unsigned slot, uint32_t v) {
    iowin_write32(&c->db_regs, slot * 4, v);
}

static inline uint32_t portsc_off(unsigned port1) {
    return XOP_PORTSC_BASE + (port1 - 1) * 0x10;
}
static inline uint32_t portsc_rd(struct xhci_hc *c, unsigned port1) { return op_rd(c, portsc_off(port1)); }
static inline void portsc_wr(struct xhci_hc *c, unsigned port1, uint32_t set) {
    op_wr(c, portsc_off(port1), (portsc_rd(c, port1) & XPORT_PRESERVE) | set);
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
static void file_transfer(struct xhci_hc *c, const struct xhci_trb *ev) {
    unsigned es = TRB_EV_SLOT(ev->control);
    if (es >= 1 && es <= MAX_SLOTS_USED) {
        c->int_done[es] = 1;
        c->int_done_status[es] = ev->status;
    }
}

static int event_matches(const struct xhci_trb *ev, const struct ev_want *w) {
    if ((uint8_t)TRB_GET_TYPE(ev->control) != w->type) return 0;
    if (w->slot && TRB_EV_SLOT(ev->control) != w->slot) return 0;
    if (w->dci && TRB_EV_DCI(ev->control) != w->dci) return 0;
    if (w->trb && (uint32_t)ev->parameter != w->trb) return 0;
    return 1;
}

/* Take the next event if the controller has written one, and tell the
   controller how far we have consumed. Bit 3 of ERDP is EHB (event handler
   busy) and is write-1-to-clear. */
static int take_event(struct xhci_hc *c, struct xhci_trb *copy) {
    struct xhci_trb *e = &c->evt_ring[c->evt_deq];
    uint32_t ctrl = e->control;
    if ((ctrl & TRB_CYCLE) != c->evt_cycle) return 0;
    copy->parameter = e->parameter;
    copy->status = e->status;
    copy->control = ctrl;

    if (++c->evt_deq == EVT_RING_TRBS) { c->evt_deq = 0; c->evt_cycle ^= 1; }
    rt_wr64(c, 0x20 + 0x18,
            (dma_phys(c, EVTRING_OFF) + c->evt_deq * sizeof(struct xhci_trb)) | (1u << 3));
    return 1;
}

static int wait_event(struct xhci_hc *c, const struct ev_want *want, struct xhci_trb *out,
                      int timeout_ms) {
    for (int ms = 0; ms < timeout_ms; ms++) {
        struct xhci_trb copy;
        if (take_event(c, &copy)) {
            if (event_matches(&copy, want)) { if (out) *out = copy; return 1; }
            /* Port status change events arrive unsolicited while we work; they
               are consumed above and otherwise ignored. */
            if ((uint8_t)TRB_GET_TYPE(copy.control) == TRB_EV_TRANSFER) file_transfer(c, &copy);
            continue;
        }
        timer_sleep(1);
    }
    return 0;
}

/* Check the event ring once, without sleeping. Same consumer-cycle logic as
   wait_event, but returns immediately if nothing new has landed - which is what
   an interrupt-endpoint poll needs. */
static int poll_event(struct xhci_hc *c, uint8_t want_type, struct xhci_trb *out) {
    struct xhci_trb copy;
    if (!take_event(c, &copy)) return 0;
    if ((uint8_t)TRB_GET_TYPE(copy.control) != want_type) return 0;
    if (out) *out = copy;
    return 1;
}

/* Post a command and wait for its completion event. Returns the completion
   code, or 0 on timeout (0 is "Invalid", never a real success). */
static uint8_t run_command(struct xhci_hc *c, uint64_t param, uint32_t status, uint32_t control,
                           struct xhci_trb *out) {
    uint32_t trb_addr = dma_phys(c, CMDRING_OFF) + c->cmd_enq * (uint32_t)sizeof(struct xhci_trb);
    struct xhci_trb *t = &c->cmd_ring[c->cmd_enq];
    t->parameter = param;
    t->status = status;
    /* The cycle bit is written LAST as part of control: it is what hands the
       TRB to the controller, so everything else must already be visible. */
    t->control = control | (c->cmd_cycle ? TRB_CYCLE : 0);

    if (++c->cmd_enq == CMD_RING_TRBS - 1) {
        /* The final TRB is a Link back to the start; toggling the cycle there
           is what makes the ring circular rather than a buffer we overrun. */
        struct xhci_trb *link = &c->cmd_ring[CMD_RING_TRBS - 1];
        link->parameter = dma_phys(c, CMDRING_OFF);
        link->status = 0;
        link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (c->cmd_cycle ? TRB_CYCLE : 0);
        c->cmd_enq = 0;
        c->cmd_cycle ^= 1;
    }

    doorbell(c, 0, 0);      /* doorbell 0 is the command ring */

    struct xhci_trb ev;
    struct ev_want w = { TRB_EV_CMD_COMPLETE, 0, 0, trb_addr };
    if (!wait_event(c, &w, &ev, 1000)) return 0;
    if (out) *out = ev;
    return (uint8_t)TRB_CC(ev.status);
}

/* ---- contexts -----------------------------------------------------------
 * Context entries are 32 or 64 bytes depending on HCCPARAMS1.CSZ, which is why
 * every context access is computed rather than indexed as an array. Assuming 32
 * on a 64-byte controller puts the endpoint context at the wrong offset and the
 * Address Device command fails with a parameter error. */
static volatile uint32_t *ctx_at(struct xhci_hc *c, uint32_t base_off, unsigned index) {
    return (volatile uint32_t *)((uint8_t *)dma_virt(c, base_off) + index * c->ctx_size);
}

static uint32_t devctx_off(unsigned slot) { return DEVCTX_OFF + (slot - 1) * 0x1000; }
static uint32_t ep0ring_off(unsigned slot) { return EP0RING_OFF + (slot - 1) * 256; }

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
static void build_input_context(struct xhci_hc *c, struct usb_device *dev, uint32_t mps0,
                                int for_eval) {
    unsigned slot = dev->slot;

    /* zero the whole input context - stale fields are interpreted, not ignored */
    volatile uint8_t *raw = dma_virt(c, INPUTCTX_OFF);
    for (unsigned i = 0; i < c->ctx_size * 3; i++) raw[i] = 0;

    volatile uint32_t *icc = ctx_at(c, INPUTCTX_OFF, 0);   /* input control    */
    volatile uint32_t *sc  = ctx_at(c, INPUTCTX_OFF, 1);   /* slot context     */
    volatile uint32_t *ep0 = ctx_at(c, INPUTCTX_OFF, 2);   /* EP0 context      */

    /* Add flags: bit0 = slot context, bit1 = EP0. Evaluate Context only needs
       EP0 (we are correcting the packet size), Address Device needs both. */
    icc[1] = for_eval ? (1u << 1) : ((1u << 0) | (1u << 1));

    /* slot: route string 0 (root port, no hubs), speed, one context entry */
    sc[0] = (xspeed_to_id(dev->speed) << 20) | (1u << 27);
    sc[1] = ((uint32_t)dev->port << 16);

    /* EP0: control endpoint, three retries, the transfer ring with its cycle */
    ep0[1] = (4u << 3) | (3u << 1) | (mps0 << 16);
    uint32_t ring = dma_phys(c, ep0ring_off(slot));
    ep0[2] = ring | 1u;          /* dequeue pointer | DCS */
    ep0[3] = 0;
    ep0[4] = 8;                  /* average TRB length */
}

/* ---- control transfers -------------------------------------------------- */
static int xhci_control(struct usb_device *dev, uint8_t bm_request_type,
                        uint8_t b_request, uint16_t w_value, uint16_t w_index,
                        void *data, uint16_t w_length) {
    if (!dev || !dev->hc || dev->slot == 0) return -1;
    if (w_length > XFER_MAX) return -1;
    struct xhci_hc *c = dev->hc;

    unsigned slot = dev->slot;
    struct xhci_trb *ring = dma_virt(c, ep0ring_off(slot));
    int dev_to_host = (bm_request_type & 0x80) != 0;

    if (!dev_to_host && data && w_length)
        for (uint16_t i = 0; i < w_length; i++) c->xfer_buf[i] = ((const uint8_t *)data)[i];

    unsigned i = c->ep0_enq[slot];
    unsigned cyc = c->ep0_cycle[slot];

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
        t->parameter = dma_phys(c, XFERBUF_OFF);
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
        link->parameter = dma_phys(c, ep0ring_off(slot));
        link->status = 0;
        link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (cyc ? TRB_CYCLE : 0);
        i = 0; cyc ^= 1;
    }
    c->ep0_enq[slot] = i;
    c->ep0_cycle[slot] = cyc;

    doorbell(c, slot, 1);       /* DCI 1 = EP0 */

    struct xhci_trb ev;
    struct ev_want w = { TRB_EV_TRANSFER, (uint8_t)slot, 1, 0 };
    if (!wait_event(c, &w, &ev, 1000)) return -1;
    uint8_t cc = (uint8_t)TRB_CC(ev.status);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) return -1;

    /* On a transfer event the low 24 bits of status are the RESIDUAL - what was
       not transferred - so the actual count is the request minus that. */
    int residual = (int)(ev.status & 0x00FFFFFF);
    int transferred = (int)w_length - residual;
    if (transferred < 0) transferred = 0;
    if (transferred > (int)w_length) transferred = (int)w_length;

    if (dev_to_host && data && transferred > 0)
        for (int b = 0; b < transferred; b++) ((uint8_t *)data)[b] = c->xfer_buf[b];

    return transferred;
}

/* ---- attach: enable a slot and let the controller address the device ----- */
static int xhci_attach(struct usb_device *dev) {
    struct xhci_hc *c = dev->hc;
    struct xhci_trb ev;

    uint8_t cc = run_command(c, 0, 0, TRB_TYPE(TRB_CMD_ENABLE_SLOT), &ev);
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
    volatile uint8_t *dc = dma_virt(c, devctx_off(slot));
    for (unsigned i = 0; i < c->ctx_size * 32; i++) dc[i] = 0;
    c->dcbaa[slot] = dma_phys(c, devctx_off(slot));

    /* EP0 transfer ring */
    struct xhci_trb *ring = dma_virt(c, ep0ring_off(slot));
    for (unsigned i = 0; i < EP0_RING_TRBS; i++) trb_clear(&ring[i]);
    c->ep0_enq[slot] = 0;
    c->ep0_cycle[slot] = 1;
    c->bulk_configured[slot][0] = c->bulk_configured[slot][1] = 0;
    c->max_dci[slot] = 1;
    c->int_configured[slot] = 0;
    c->int_pending[slot] = 0;
    c->int_done[slot] = 0;

    uint32_t mps0 = speed_to_mps0(dev->speed);
    build_input_context(c, dev, mps0, 0);

    cc = run_command(c, dma_phys(c, INPUTCTX_OFF), 0,
                     TRB_TYPE(TRB_CMD_ADDRESS_DEV) | ((uint32_t)slot << 24), &ev);
    if (cc != TRB_CC_SUCCESS) {
        klog_u32("xHCI", SEV_WARN, "Address Device failed, completion code ", cc,
                 LOG_COLOR_VALUE, "");
        return 0;
    }

    /* The controller chose the address and wrote it into the output slot
       context; read it back rather than assuming. */
    volatile uint32_t *out_slot = ctx_at(c, devctx_off(slot), 0);
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
        build_input_context(c, dev, dd.b_max_packet_size0, 1);
        run_command(c, dma_phys(c, INPUTCTX_OFF), 0,
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

static uint32_t bulkring_off(unsigned slot, int in) {
    return BULKRING_OFF + ((slot - 1) * 2 + (in ? 1 : 0)) * 256;
}

#define BULK_RING_TRBS 16

/* ep_type is the xHCI endpoint type: 2 = Bulk OUT, 6 = Bulk IN, 3 = Interrupt
   OUT, 7 = Interrupt IN. `interval` is the encoded polling interval, ignored
   for bulk. `redo` drops the endpoint and adds it back in the one command,
   which is how an endpoint that already exists is put back to its first
   state - toggle and all (xhci_reset_ep). `ring` is the ring's offset in the
   controller's DMA pages. */
static int configure_ep(struct xhci_hc *c, struct usb_device *dev, uint8_t ep, uint16_t mps,
                        uint32_t ring, unsigned ep_type, uint32_t interval, int redo) {
    unsigned slot = dev->slot;
    unsigned dci = dci_for(ep);
    if (dci > c->max_dci[slot]) c->max_dci[slot] = (uint8_t)dci;

    /* Input context: the control context names what is being added, then the
       slot context (whose "context entries" must reach the highest DCI in
       use, not just this one, or a later endpoint would cut off an earlier
       one) and the endpoint context itself. */
    volatile uint8_t *raw = dma_virt(c, INPUTCTX_OFF);
    for (unsigned i = 0; i < c->ctx_size * 32; i++) raw[i] = 0;

    volatile uint32_t *icc = ctx_at(c, INPUTCTX_OFF, 0);
    volatile uint32_t *sc  = ctx_at(c, INPUTCTX_OFF, 1);
    volatile uint32_t *epc = ctx_at(c, INPUTCTX_OFF, dci + 1);

    if (redo) icc[0] = 1u << dci;           /* drop it ... */
    icc[1] = (1u << 0) | (1u << dci);       /* ... and add: slot + this endpoint */
    sc[0] = (xspeed_to_id(dev->speed) << 20) | ((uint32_t)c->max_dci[slot] << 27);
    sc[1] = ((uint32_t)dev->port << 16);

    epc[0] = interval << 16;
    epc[1] = ((uint32_t)ep_type << 3) | (3u << 1) | ((uint32_t)mps << 16);
    epc[2] = dma_phys(c, ring) | 1u;        /* dequeue pointer | DCS */
    epc[3] = 0;
    epc[4] = mps;                           /* average TRB length */

    struct xhci_trb ev;
    uint8_t cc = run_command(c, dma_phys(c, INPUTCTX_OFF), 0,
                             TRB_TYPE(TRB_CMD_CONFIG_EP) | ((uint32_t)slot << 24), &ev);
    if (cc != TRB_CC_SUCCESS) {
        klog_u32("xHCI", SEV_WARN, "Configure Endpoint failed, completion code ", cc,
                 LOG_COLOR_VALUE, "");
        return 0;
    }
    return 1;
}

static int xhci_bulk(struct usb_device *dev, uint8_t ep, void *data, uint32_t len) {
    if (!dev || !dev->hc || dev->slot == 0 || !data) return -1;
    if (len == 0 || len > XFER_MAX) return -1;
    struct xhci_hc *c = dev->hc;

    unsigned slot = dev->slot;
    int in = (ep & 0x80) != 0;
    uint16_t mps = in ? dev->ep_in_mps : dev->ep_out_mps;
    if (mps == 0) mps = 512;

    if (!c->bulk_configured[slot][in]) {
        struct xhci_trb *r = dma_virt(c, bulkring_off(slot, in));
        for (unsigned i = 0; i < BULK_RING_TRBS; i++) trb_clear(&r[i]);
        c->bulk_enq[slot][in] = 0;
        c->bulk_cycle[slot][in] = 1;
        if (!configure_ep(c, dev, ep, mps, bulkring_off(slot, in), in ? 6u : 2u, 0, 0))
            return -1;
        c->bulk_configured[slot][in] = 1;
    }

    if (!in) for (uint32_t i = 0; i < len; i++) c->xfer_buf[i] = ((const uint8_t *)data)[i];

    struct xhci_trb *ring = dma_virt(c, bulkring_off(slot, in));
    unsigned i = c->bulk_enq[slot][in];
    unsigned cyc = c->bulk_cycle[slot][in];
    uint32_t trb_addr = dma_phys(c, bulkring_off(slot, in)) + i * (uint32_t)sizeof(struct xhci_trb);

    struct xhci_trb *t = &ring[i];
    t->parameter = dma_phys(c, XFERBUF_OFF);
    t->status = len;
    /* ISP so a short packet completes rather than being an error - a CSW read
       is routinely shorter than the buffer offered for it. */
    t->control = TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP | (cyc ? TRB_CYCLE : 0);

    if (++i == BULK_RING_TRBS - 1) {
        struct xhci_trb *link = &ring[BULK_RING_TRBS - 1];
        link->parameter = dma_phys(c, bulkring_off(slot, in));
        link->status = 0;
        link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (cyc ? TRB_CYCLE : 0);
        i = 0; cyc ^= 1;
    }
    c->bulk_enq[slot][in] = i;
    c->bulk_cycle[slot][in] = cyc;

    doorbell(c, slot, dci_for(ep));

    struct xhci_trb ev;
    struct ev_want w = { TRB_EV_TRANSFER, (uint8_t)slot, (uint8_t)dci_for(ep), trb_addr };
    if (!wait_event(c, &w, &ev, 2000)) return -1;
    uint8_t cc = (uint8_t)TRB_CC(ev.status);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) return -1;

    int residual = (int)(ev.status & 0x00FFFFFF);
    int transferred = (int)len - residual;
    if (transferred < 0) transferred = 0;
    if (transferred > (int)len) transferred = (int)len;

    if (in && transferred > 0)
        for (int b = 0; b < transferred; b++) ((uint8_t *)data)[b] = c->xfer_buf[b];
    return transferred;
}

static uint32_t intring_off(unsigned slot) { return INTRING_OFF + (slot - 1) * 256; }
static uint32_t intbuf_off(unsigned slot)  { return INTBUF_OFF  + (slot - 1) * 256; }

static int xhci_interrupt_poll(struct usb_device *dev, uint8_t ep,
                               void *data, uint32_t len) {
    if (!dev || !dev->hc || dev->slot == 0 || !data) return -1;
    if (len == 0 || len > 64) return -1;
    struct xhci_hc *c = dev->hc;

    unsigned slot = dev->slot;
    uint16_t mps = dev->ep_int_mps ? dev->ep_int_mps : 8;

    if (!c->int_configured[slot]) {
        struct xhci_trb *r = dma_virt(c, intring_off(slot));
        for (unsigned i = 0; i < BULK_RING_TRBS; i++) trb_clear(&r[i]);
        c->int_enq[slot] = 0;
        c->int_cycle[slot] = 1;
        /* bInterval for a high-speed interrupt endpoint is already the
           2^(n-1) microframe exponent xHCI wants; a full-speed one counts
           frames, and 3 (8 microframes = 1 ms) is the usual mapping. */
        uint32_t interval = dev->speed == USB_SPEED_HIGH
                          ? (dev->ep_int_interval ? dev->ep_int_interval - 1 : 3) : 3;
        if (!configure_ep(c, dev, ep, mps, intring_off(slot), 7u, interval, 0)) return -1;
        c->int_configured[slot] = 1;
        c->int_pending[slot] = 0;
    }

    if (!c->int_pending[slot]) {
        struct xhci_trb *ring = dma_virt(c, intring_off(slot));
        unsigned i = c->int_enq[slot], cyc = c->int_cycle[slot];
        struct xhci_trb *t = &ring[i];
        t->parameter = dma_phys(c, intbuf_off(slot));
        t->status = len;
        t->control = TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP | (cyc ? TRB_CYCLE : 0);
        if (++i == BULK_RING_TRBS - 1) {
            struct xhci_trb *link = &ring[BULK_RING_TRBS - 1];
            link->parameter = dma_phys(c, intring_off(slot));
            link->status = 0;
            link->control = TRB_TYPE(TRB_LINK) | TRB_ENT | (cyc ? TRB_CYCLE : 0);
            i = 0; cyc ^= 1;
        }
        c->int_enq[slot] = i;
        c->int_cycle[slot] = cyc;
        c->int_len[slot] = len;
        doorbell(c, slot, dci_for(ep));
        c->int_pending[slot] = 1;
        return 0;
    }

    /* Drain whatever has landed and file each completion under its own slot,
       then look only at ours. */
    struct xhci_trb ev;
    while (poll_event(c, TRB_EV_TRANSFER, &ev)) file_transfer(c, &ev);
    if (!c->int_done[slot]) return 0;                   /* nothing for us yet */
    c->int_done[slot] = 0;
    c->int_pending[slot] = 0;

    uint32_t status = c->int_done_status[slot];
    uint8_t cc = (uint8_t)TRB_CC(status);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PKT) return -1;

    int residual = (int)(status & 0x00FFFFFF);
    int got = (int)c->int_len[slot] - residual;
    if (got < 0) got = 0;
    if (got > (int)len) got = (int)len;
    const uint8_t *src = dma_virt(c, intbuf_off(slot));
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
    if (!dev || !dev->hc || dev->slot == 0) return;
    struct xhci_hc *c = dev->hc;
    unsigned slot = dev->slot;
    int bulk = (ep & 0x0F) != 0;
    int in = (ep & 0x80) != 0;
    if (bulk && !c->bulk_configured[slot][in]) return;   /* nothing ever ran on it */
    unsigned dci = bulk ? dci_for(ep) : 1;

    /* The OUTPUT device context: the slot context, then one per DCI. */
    volatile uint32_t *epc = ctx_at(c, devctx_off(slot), dci);
    uint32_t state = epc[0] & 7u;
    if (state == EP_STATE_DISABLED) return;
    uint32_t id = ((uint32_t)slot << 24) | ((uint32_t)dci << 16);
    if (state == EP_STATE_HALTED)
        run_command(c, 0, 0, TRB_TYPE(TRB_CMD_RESET_EP) | id, 0);
    else if (state == EP_STATE_RUNNING)
        run_command(c, 0, 0, TRB_TYPE(TRB_CMD_STOP_EP) | id, 0);

    if (!bulk) {
        uint32_t deq = dma_phys(c, ep0ring_off(slot))
                     + c->ep0_enq[slot] * (uint32_t)sizeof(struct xhci_trb);
        run_command(c, deq | (c->ep0_cycle[slot] ? 1u : 0u), 0,
                    TRB_TYPE(TRB_CMD_SET_TR_DEQ) | id, 0);
        return;
    }
    struct xhci_trb *r = dma_virt(c, bulkring_off(slot, in));
    for (unsigned i = 0; i < BULK_RING_TRBS; i++) trb_clear(&r[i]);
    c->bulk_enq[slot][in] = 0;
    c->bulk_cycle[slot][in] = 1;
    uint16_t mps = in ? dev->ep_in_mps : dev->ep_out_mps;
    if (mps == 0) mps = 512;
    if (!configure_ep(c, dev, ep, mps, bulkring_off(slot, in), in ? 6u : 2u, 0, 1))
        c->bulk_configured[slot][in] = 0;   /* the next transfer configures it afresh */
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

static void enumerate_port(struct xhci_hc *c, unsigned port1) {
    uint32_t sc = portsc_rd(c, port1);
    if (!(sc & XPORT_CCS)) return;

    /* A USB 3 port reports itself enabled straight out of reset; a USB 2 port
       needs the reset driven explicitly. Doing the reset either way is
       harmless and covers both. */
    if (!(sc & XPORT_PED)) {
        portsc_wr(c, port1, XPORT_PR);
        int ok = 0;
        for (int i = 0; i < 100; i++) {
            timer_sleep(1);
            sc = portsc_rd(c, port1);
            if (sc & XPORT_PRC) { ok = 1; break; }
        }
        /* clear the reset-change flag (write 1) */
        op_wr(c, portsc_off(port1), (sc & XPORT_PRESERVE) | XPORT_PRC);
        if (!ok) {
            klog_u32("xHCI", SEV_WARN, "port ", port1, LOG_COLOR_VALUE,
                     ": reset never completed");
            return;
        }
        sc = portsc_rd(c, port1);
    }
    if (!(sc & XPORT_PED)) return;       /* nothing usable here */

    enum usb_speed sp = port_speed(sc);
    if (sp == USB_SPEED_UNKNOWN) {
        klog_u32("xHCI", SEV_WARN, "port ", port1, LOG_COLOR_VALUE,
                 ": controller reported an unknown speed id");
        return;
    }

    struct usb_device *dev = usb_alloc_device(&xhci_ops, c, (uint8_t)port1, sp);
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
 * failure here costs nothing. Run on the first controller only: one delivery
 * proves the path. */
static volatile uint32_t msi_hits = 0;
static struct xhci_hc *msi_hc;          /* the controller under test */

static void xhci_msi_handler(struct registers *r) {
    (void)r;
    msi_hits++;
    /* Acknowledge at the device as well as the APIC: the interrupter's pending
       bit is write-1-to-clear and a set one suppresses the next interrupt. */
    if (msi_hc) rt_wr(msi_hc, 0x20 + 0x00, rt_rd(msi_hc, 0x20 + 0x00) | 1u);
}

static void msi_self_test(struct xhci_hc *c, const struct pci_device *dev) {
    if (!irq_msi_available()) return;

    int vector = irq_alloc_msi_vector(xhci_msi_handler);
    if (vector < 0) return;

    if (!pci_msi_setup(dev, (uint8_t)vector)) {
        irq_free_msi_vector(vector);
        klog("xHCI", SEV_INFO, "no MSI or MSI-X capability on this controller");
        return;
    }

    msi_hc = c;
    msi_hits = 0;
    rt_wr(c, 0x20 + 0x00, 0x3);                  /* interrupter 0: pending clear + enable */
    op_wr(c, XOP_USBCMD, op_rd(c, XOP_USBCMD) | (1u << 2));   /* INTE */

    struct xhci_trb ev;
    run_command(c, 0, 0, TRB_TYPE(TRB_CMD_NOOP) | TRB_IOC, &ev);

    for (int i = 0; i < 200000 && msi_hits == 0; i++) { }

    uint32_t hits = msi_hits;
    op_wr(c, XOP_USBCMD, op_rd(c, XOP_USBCMD) & ~(1u << 2));  /* back to polling */
    rt_wr(c, 0x20 + 0x00, 0x1);
    pci_msi_disable(dev);
    irq_free_msi_vector(vector);
    msi_hc = 0;

    if (hits) klog_u32("xHCI", SEV_OK, "MSI delivered on vector ",
                       (uint32_t)vector, LOG_COLOR_VALUE, "");
    else      klog_u32("xHCI", SEV_WARN, "MSI configured but never delivered, vector ",
                       (uint32_t)vector, LOG_COLOR_VALUE, "");
}

/* Bring up one controller. Returns 1 if it came up. */
static int xhci_start(struct xhci_hc *c, const struct pci_device *dev, unsigned index) {
    pci_enable_bus_master(dev);

    uint32_t base = pci_bar_mmio32(dev, 0, "xHCI");
    if (base == 0) return 0;

    /* xHCI register files are large - runtime and doorbell regions sit well
       past the operational registers - so map generously. */
    for (uint32_t off = 0; off < XHCI_MAP_SIZE; off += 0x1000)
        paging_map_kernel(base + off, base + off, VM_WRITE | VM_UNCACHED);
    if (iowin_mmio(&c->cap_regs, (volatile void *)base, XHCI_CAP_SIZE) != 0) return 0;

    uint32_t hcs1 = cap_rd(c, XCAP_HCSPARAMS1);
    uint32_t caplen = iowin_read8(&c->cap_regs, XCAP_CAPLENGTH);
    uint32_t op_size = XOP_PORTSC_BASE + HCS1_MAXPORTS(hcs1) * 0x10u;
    uint32_t rtsoff = cap_rd(c, XCAP_RTSOFF) & ~0x1Fu;
    uint32_t dboff  = cap_rd(c, XCAP_DBOFF) & ~0x3u;
    if (caplen < XHCI_CAP_SIZE || caplen + op_size > XHCI_MAP_SIZE ||
        rtsoff > XHCI_MAP_SIZE - XHCI_RT_SIZE || dboff > XHCI_MAP_SIZE - XHCI_DB_SIZE ||
        iowin_mmio(&c->op_regs, (volatile void *)(base + caplen), op_size) != 0 ||
        iowin_mmio(&c->rt_regs, (volatile void *)(base + rtsoff), XHCI_RT_SIZE) != 0 ||
        iowin_mmio(&c->db_regs, (volatile void *)(base + dboff), XHCI_DB_SIZE) != 0) {
        klog("xHCI", SEV_WARN, "register files outside the mapped BAR - not initialising");
        return 0;
    }

    uint32_t hcs2 = cap_rd(c, XCAP_HCSPARAMS2);
    uint32_t hcc1 = cap_rd(c, XCAP_HCCPARAMS1);
    c->nports = HCS1_MAXPORTS(hcs1);
    unsigned maxslots = HCS1_MAXSLOTS(hcs1);
    c->ctx_size = HCC1_CSZ(hcc1) ? 64 : 32;
    if (c->nports == 0 || maxslots == 0) return 0;

    c->dma_v = usb_dma_alloc(XDMA_PAGES, XDMA_VBASE + index * 0x20000u, &c->dma);
    if (!c->dma_v) {
        klog("xHCI", SEV_WARN, "no memory for the controller's DMA pages");
        return 0;
    }

    /* Wait for the controller to finish its own power-on before touching it. */
    for (int i = 0; i < 100 && (op_rd(c, XOP_USBSTS) & XSTS_CNR); i++) timer_sleep(1);

    op_wr(c, XOP_USBCMD, op_rd(c, XOP_USBCMD) & ~XCMD_RS);
    for (int i = 0; i < 100 && !(op_rd(c, XOP_USBSTS) & XSTS_HCH); i++) timer_sleep(1);

    op_wr(c, XOP_USBCMD, XCMD_HCRST);
    int ok = 0;
    for (int i = 0; i < 500; i++) {
        if (!(op_rd(c, XOP_USBCMD) & XCMD_HCRST) && !(op_rd(c, XOP_USBSTS) & XSTS_CNR)) {
            ok = 1; break;
        }
        timer_sleep(1);
    }
    if (!ok) { klog("xHCI", SEV_WARN, "controller reset did not complete"); return 0; }

    uint32_t spad;
    if (xhci_spad_plan(hcs2, op_rd(c, XOP_PAGESIZE), &spad) != 0) {
        klog("xHCI", SEV_WARN, "controller cannot use 4 KB pages - not initialising");
        return 0;
    }

    if (maxslots > MAX_SLOTS_USED) maxslots = MAX_SLOTS_USED;
    op_wr(c, XOP_CONFIG, maxslots);

    /* device context base address array */
    c->dcbaa = dma_virt(c, DCBAA_OFF);
    for (unsigned i = 0; i <= maxslots; i++) c->dcbaa[i] = 0;

    /* Scratchpad: memory the controller wants for its own use. Entry 0 of the
       DCBAA points at the array of pages. Skipping this on a controller that
       asks for them is a controller that never starts. */
    if (spad) {
        if (xhci_spad_build(spad, &c->spad_array) != 0) {
            klog_u32("xHCI", SEV_WARN, "no memory for the scratchpad pages asked for: ",
                     spad, LOG_COLOR_VALUE, "");
            return 0;
        }
        c->spad_count = spad;
        c->dcbaa[0] = c->spad_array;
        klog_u32("xHCI", SEV_INFO, "scratchpad pages: ", spad, LOG_COLOR_VALUE, "");
    }
    op_wr64(c, XOP_DCBAAP, dma_phys(c, DCBAA_OFF));

    /* command ring, with its trailing Link TRB back to the start */
    c->cmd_ring = dma_virt(c, CMDRING_OFF);
    for (unsigned i = 0; i < CMD_RING_TRBS; i++) trb_clear(&c->cmd_ring[i]);
    c->cmd_ring[CMD_RING_TRBS - 1].parameter = dma_phys(c, CMDRING_OFF);
    c->cmd_ring[CMD_RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_ENT;
    c->cmd_enq = 0; c->cmd_cycle = 1;
    op_wr64(c, XOP_CRCR, dma_phys(c, CMDRING_OFF) | 1u);     /* | RCS */

    /* event ring: one segment, described by a one-entry segment table */
    c->evt_ring = dma_virt(c, EVTRING_OFF);
    for (unsigned i = 0; i < EVT_RING_TRBS; i++) trb_clear(&c->evt_ring[i]);
    c->evt_deq = 0; c->evt_cycle = 1;

    volatile uint32_t *erst = dma_virt(c, ERST_OFF);
    erst[0] = dma_phys(c, EVTRING_OFF);
    erst[1] = 0;
    erst[2] = EVT_RING_TRBS;
    erst[3] = 0;

    /* Interrupter 0 lives at runtime offset 0x20. ERDP must be set BEFORE
       ERSTBA: writing the segment table base is what arms the interrupter. */
    rt_wr(c, 0x20 + 0x08, 1);                               /* ERSTSZ = 1 segment */
    rt_wr64(c, 0x20 + 0x18, dma_phys(c, EVTRING_OFF));      /* ERDP               */
    rt_wr64(c, 0x20 + 0x10, dma_phys(c, ERST_OFF));         /* ERSTBA             */

    c->xfer_buf = dma_virt(c, XFERBUF_OFF);

    op_wr(c, XOP_USBCMD, XCMD_RS);
    for (int i = 0; i < 100 && (op_rd(c, XOP_USBSTS) & XSTS_HCH); i++) timer_sleep(1);
    if (op_rd(c, XOP_USBSTS) & XSTS_HCH) {
        klog("xHCI", SEV_WARN, "controller would not leave the halted state");
        xhci_spad_release(c->spad_array, c->spad_count);   /* halted: nobody uses them */
        return 0;
    }

    klog_u32("xHCI", SEV_OK, "controller online, root ports: ", c->nports, LOG_COLOR_VALUE, "");
    klog_child_u32("  pci device ", dev->slot, LOG_COLOR_VALUE, "");

    if (index == 0) msi_self_test(c, dev);

    /* Power every port, then let devices settle before looking. */
    for (unsigned p = 1; p <= c->nports; p++)
        if (!(portsc_rd(c, p) & XPORT_PP)) portsc_wr(c, p, XPORT_PP);
    timer_sleep(100);

    for (unsigned p = 1; p <= c->nports; p++) enumerate_port(c, p);
    return 1;
}

int xhci_init(void) {
    nhcs = 0;
    /* class 0x0C serial bus, subclass 0x03 USB, prog-if 0x30 = xHCI. Every
       one of them: each has its own ports. */
    for (unsigned k = 0; nhcs < XHCI_MAX_HCS; k++) {
        const struct pci_device *dev = pci_find(0x0C, 0x03, 0x30, k);
        if (!dev) break;
        struct xhci_hc *c = &hcs[nhcs];
        for (unsigned i = 0; i < sizeof(*c); i++) ((uint8_t *)c)[i] = 0;
        if (xhci_start(c, dev, nhcs)) nhcs++;
    }
    return (int)nhcs;
}
