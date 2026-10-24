// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/usb/ehci.c */
/* Aurora Tejeda / CATX Systems */
/* EHCI host controller, stage 1. See ehci.h for scope and for why a USB
   keyboard is deliberately not this driver's job. */

#include "ehci.h"
#include "usb.h"
#include "pci.h"
#include "arch/paging.h"
#include "logging.h"
#include "timer.h"
#include "drivers/iowin.h"

/* ---- capability registers (at BAR0) ------------------------------------- */
#define CAP_CAPLENGTH   0x00   /* 8-bit: distance from BAR0 to the op regs   */
#define CAP_HCSPARAMS   0x04
#define CAP_HCCPARAMS   0x08

#define HCSPARAMS_NPORTS(x)  ((x) & 0x0F)
#define EHCI_MAX_PORTS       15       /* NPORTS is four bits */
#define HCSPARAMS_PPC(x)     (((x) >> 4) & 1)     /* port power control      */
#define HCCPARAMS_EECP(x)    (((x) >> 8) & 0xFF)  /* PCI cfg offset of the
                                                     legacy-support capability */

/* ---- operational registers (at BAR0 + CAPLENGTH) ------------------------ */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_USBINTR      0x08
#define OP_FRINDEX      0x0C
#define OP_CTRLDSSEG    0x10
#define OP_PERIODICLIST 0x14
#define OP_ASYNCLIST    0x18
#define OP_CONFIGFLAG   0x40
#define OP_PORTSC       0x44   /* array, one dword per port                  */

#define CMD_RS          (1u << 0)    /* run/stop                             */
#define CMD_HCRESET     (1u << 1)
#define CMD_PSE         (1u << 4)    /* periodic schedule enable             */
#define CMD_ASE         (1u << 5)    /* async schedule enable                */

#define STS_HCHALTED    (1u << 12)
#define STS_ASS         (1u << 15)   /* async schedule status: really running */

/* PORTSC. Bits 1, 3 and 5 are write-1-to-CLEAR: a read-modify-write that does
   not mask them off will clear change flags as a side effect of any unrelated
   port write, and the connect event simply vanishes. */
#define PORTSC_CCS      (1u << 0)    /* current connect status               */
#define PORTSC_CSC      (1u << 1)    /* connect status change       (RWC)    */
#define PORTSC_PED      (1u << 2)    /* port enabled                         */
#define PORTSC_PEC      (1u << 3)    /* port enable change          (RWC)    */
#define PORTSC_OCC      (1u << 5)    /* over-current change         (RWC)    */
#define PORTSC_RESET    (1u << 8)
#define PORTSC_LINESTS  (3u << 10)
#define PORTSC_LS_KSTATE (1u << 10)  /* 01b = K-state = low-speed device     */
#define PORTSC_POWER    (1u << 12)
#define PORTSC_OWNER    (1u << 13)   /* 1 = hand this port to the companion  */
#define PORTSC_RWC_MASK (PORTSC_CSC | PORTSC_PEC | PORTSC_OCC)

/* ---- transfer descriptors ----------------------------------------------- */
#define QTD_T           1u           /* terminate: pointer is invalid        */

#define QTD_STS_ACTIVE  (1u << 7)
#define QTD_STS_HALTED  (1u << 6)
#define QTD_STS_BUFERR  (1u << 5)
#define QTD_STS_BABBLE  (1u << 4)
#define QTD_STS_XACTERR (1u << 3)
#define QTD_STS_ERRMASK (QTD_STS_HALTED | QTD_STS_BUFERR | QTD_STS_BABBLE | QTD_STS_XACTERR)

#define QTD_PID_OUT     (0u << 8)
#define QTD_PID_IN      (1u << 8)
#define QTD_PID_SETUP   (2u << 8)
#define QTD_CERR_3      (3u << 10)   /* retry three times before halting     */
#define QTD_IOC         (1u << 15)
#define QTD_LEN(n)      (((uint32_t)(n) & 0x7FFF) << 16)
#define QTD_TOGGLE      (1u << 31)

/* qTD: exactly 32 bytes, and the pool stride must stay 32-byte aligned. The
   controller masks off the low 5 bits of any qTD pointer, so a struct that is
   not a multiple of 32 does not fail loudly - entry 1 silently gets fetched
   from entry 0's tail and the transfer reads garbage. Do not "pad" this.
   The five buffer pointers are page pointers: only buffer[0] carries an
   offset, the rest continue on page boundaries. */
struct ehci_qtd {
    volatile uint32_t next;
    volatile uint32_t alt_next;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
} __attribute__((packed));

/* QH: 48 bytes of architecture, padded to 64 for alignment. The overlay area
   from `current` onward is scribbled on by the controller as it works - it is
   not ours to read except for the status bits. */
struct ehci_qh {
    volatile uint32_t horizontal;    /* next QH | typ | T                    */
    volatile uint32_t chars;         /* endpoint characteristics             */
    volatile uint32_t caps;          /* endpoint capabilities                */
    volatile uint32_t current;
    volatile uint32_t next;          /* --- overlay ---                      */
    volatile uint32_t alt_next;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
    volatile uint32_t pad[4];
} __attribute__((packed));

/* The hardware defines both of these, and both are masked by the controller.
   A silent size change is a silent data-corruption bug, so make it a build
   failure instead. */
_Static_assert(sizeof(struct ehci_qtd) == 32, "qTD must be exactly 32 bytes");
_Static_assert(sizeof(struct ehci_qh) % 32 == 0, "QH stride must stay 32-byte aligned");

#define QH_CHARS_ADDR(a)    ((uint32_t)(a) & 0x7F)
#define QH_CHARS_EP(e)      (((uint32_t)(e) & 0x0F) << 8)
#define QH_CHARS_EPS_HIGH   (2u << 12)   /* 10b = high speed                 */
#define QH_CHARS_DTC        (1u << 14)   /* take the data toggle from the qTD,
                                            not from the QH - required when we
                                            drive the toggle ourselves        */
#define QH_CHARS_HEAD       (1u << 15)   /* head of the reclamation list      */
#define QH_CHARS_MPL(m)     (((uint32_t)(m) & 0x7FF) << 16)
#define QH_CAPS_MULT1       (1u << 30)   /* one transaction per uframe. Zero
                                            here means "no transactions" and
                                            the queue silently never runs.    */
#define LINK_TYP_QH         (1u << 1)

/* ---- DMA memory ---------------------------------------------------------
 * Each controller has its own eight pages (usb_dma_alloc): the controller
 * bus-masters from their PHYSICAL addresses while the CPU reaches the same
 * structures through a KERNEL-half alias, so they stay visible when CR3 points
 * at a process directory. The offsets below are within those pages. */
#define EHCI_DMA_VBASE  0xE3000000u     /* controller i's alias: + i * 0x10000 */
#define EHCI_DMA_PAGES  8

#define PFLIST_OFF      0x0000          /* 1024 dwords, 4K aligned */
#define QH_OFF          0x1000
#define QTD_OFF         0x2000
#define XFER_BUF_OFF    0x3000          /* EHCI_XFER_MAX, page aligned */
/* One queue head, qTD and buffer PER interrupt endpoint - not one shared set.
   They are separate from the control/bulk queue head so a transfer between two
   polls cannot overwrite a report in flight, and separate from each other
   because a keyboard and a mouse are both interrupt endpoints and a single
   shared set makes them clobber one another. */
#define MAX_INT_EPS     4
#define INT_QH_OFF(i)   (0x5000 + (uint32_t)(i) * 0x80)
#define INT_QTD_OFF(i)  (0x6000 + (uint32_t)(i) * 0x40)
#define INT_BUF_OFF(i)  (0x7000 + (uint32_t)(i) * 0x100)

#define NUM_QTD 4
#define MAX_TOGGLES 16

/* An AMD SB950 has three EHCI controllers and an SB850 two, each with its own
   ports; a driver that brings up only the first leaves a stick in any other
   port invisible. */
#define EHCI_MAX_HCS    4

/* BAR 0 is 256 bytes: the capability registers, then from CAPLENGTH on the
   operational ones, each reached through its own window */
#define EHCI_BAR_SIZE 0x100u

/* Everything one controller needs. A usb_device's `hc` points at its
   controller's, so the transfer functions below never have to guess. */
struct ehci_hc {
    struct iowin cap_regs;
    struct iowin op_regs;
    unsigned nports;
    int port_power_control;
    uint8_t next_address;           /* USB addresses are per bus */

    uint32_t dma;                   /* physical base of the DMA pages */
    uint8_t *dma_v;                 /* the CPU's alias of them        */
    struct ehci_qh  *qh;
    struct ehci_qtd *qtd;
    uint8_t *xfer_buf;

    struct { uint8_t addr, ep, toggle; } toggles[MAX_TOGGLES];
    struct {
        uint8_t addr, ep, armed;
        uint16_t len;
    } int_eps[MAX_INT_EPS];
    int int_list_live;
    uint16_t seen;                  /* ports already found connected and tried */
};

static struct ehci_hc hcs[EHCI_MAX_HCS];
static unsigned nhcs = 0;
static unsigned released = 0;   /* ports handed to a companion, for usb_summary */
unsigned ehci_released(void) { return released; }

static inline uint32_t dma_phys(const struct ehci_hc *c, uint32_t off) { return c->dma + off; }
static inline void    *dma_virt(const struct ehci_hc *c, uint32_t off) { return c->dma_v + off; }

static inline uint32_t cap_rd32(struct ehci_hc *c, uint32_t o) { return iowin_read32(&c->cap_regs, o); }
static inline uint8_t  cap_rd8(struct ehci_hc *c, uint32_t o)  { return iowin_read8(&c->cap_regs, o); }
static inline uint32_t op_rd(struct ehci_hc *c, uint32_t o)    { return iowin_read32(&c->op_regs, o); }
static inline void     op_wr(struct ehci_hc *c, uint32_t o, uint32_t v) { iowin_write32(&c->op_regs, o, v); }

static inline uint32_t portsc_rd(struct ehci_hc *c, unsigned p) { return op_rd(c, OP_PORTSC + p * 4); }

/* Write PORTSC without disturbing the write-1-to-clear change bits. Every port
   write in this driver goes through here for that reason. */
static inline void portsc_wr(struct ehci_hc *c, unsigned p, uint32_t v) {
    op_wr(c, OP_PORTSC + p * 4, v & ~PORTSC_RWC_MASK);
}

/* ---- BIOS handoff -------------------------------------------------------
 * The EECP field points at a PCI capability (id 1, USB Legacy Support) holding
 * two dwords: USBLEGSUP with a BIOS-owned and an OS-owned bit, and
 * USBLEGCTLSTS with the SMI enables.
 *
 * The protocol is: set OS-owned, then wait for the firmware to drop BIOS-owned.
 * Until it does, the firmware is still taking SMIs on this controller and
 * servicing it from behind our back - two drivers on one controller, which
 * presents as wildly inconsistent behaviour rather than a clean failure.
 *
 * QEMU reports EECP = 0 and skips all of this, so this path is exercised only
 * on real hardware. It is written conservatively for that reason: bounded wait,
 * and on timeout we force the issue rather than refusing to start, because a
 * firmware that never clears the bit is common enough that giving up would
 * leave USB dead on those boards. */
static void ehci_bios_handoff(const struct pci_device *dev, uint32_t hccparams) {
    uint8_t eecp = (uint8_t)HCCPARAMS_EECP(hccparams);
    if (eecp < 0x40) return;            /* below 0x40 is not a valid cap offset */

    uint32_t legsup = pci_config_read32(dev->bus, dev->slot, dev->func, eecp);
    if ((legsup & 0xFF) != 0x01) return;   /* not the legacy-support capability */

    if (!(legsup & (1u << 16))) return;    /* BIOS does not own it; nothing to do */

    klog("EHCI", SEV_INFO, "requesting controller ownership from firmware");
    pci_config_write32(dev->bus, dev->slot, dev->func, eecp, legsup | (1u << 24));

    /* Up to 1 s, as Linux waits (ehci_bios_handoff in pci-quirks.c). It was
       250 ms, and a firmware still servicing the controller after a forced
       takeover is two drivers on one stick - the FX found its stick only on
       some boots. */
    for (int i = 0; i < 1000; i++) {
        legsup = pci_config_read32(dev->bus, dev->slot, dev->func, eecp);
        if (!(legsup & (1u << 16)) && (legsup & (1u << 24))) {
            klog("EHCI", SEV_OK, "firmware released the controller");
            goto disable_smi;
        }
        timer_sleep(1);
    }

    klog("EHCI", SEV_WARN, "firmware did not release the controller - taking it anyway");
    legsup = pci_config_read32(dev->bus, dev->slot, dev->func, eecp);
    legsup &= ~(1u << 16);
    legsup |= (1u << 24);
    pci_config_write32(dev->bus, dev->slot, dev->func, eecp, legsup);

disable_smi:
    /* Silence every SMI source on this controller. Leaving them enabled means
       the firmware still traps on our own register writes. */
    pci_config_write32(dev->bus, dev->slot, dev->func, eecp + 4, 0);
}

/* ---- control transfers --------------------------------------------------
 * Every USB control transfer is three stages: a SETUP, an optional DATA, and a
 * STATUS that runs in the OPPOSITE direction to the data. The data toggle is
 * fixed by the spec rather than free: SETUP is always DATA0, and both the first
 * data packet and the status stage are DATA1. */
/* Physical address of pool entry i. A file-scope function rather than one
   nested in the caller: GCC implements a nested function as a trampoline on the
   stack, which needs an executable stack to call through. */
static uint32_t qtd_phys(const struct ehci_hc *c, int i) {
    return dma_phys(c, QTD_OFF) + (uint32_t)i * (uint32_t)sizeof(struct ehci_qtd);
}

static void qtd_clear(struct ehci_qtd *t) {
    t->next = QTD_T;
    t->alt_next = QTD_T;
    t->token = 0;
    for (int i = 0; i < 5; i++) t->buffer[i] = 0;
}

static void qtd_buffers(struct ehci_qtd *t, uint32_t phys, uint16_t len) {
    if (len == 0) return;
    t->buffer[0] = phys;
    /* buffer[1..] are PAGE pointers continuing the transfer. One extra entry
       covers any transfer up to EHCI_XFER_MAX starting in a page. */
    t->buffer[1] = (phys & ~0xFFFu) + 0x1000;
}

#define EHCI_XFER_MAX 4096

static int ehci_control(struct usb_device *dev, uint8_t bm_request_type,
                        uint8_t b_request, uint16_t w_value, uint16_t w_index,
                        void *data, uint16_t w_length) {
    if (!dev || !dev->hc) return -1;
    if (w_length > EHCI_XFER_MAX) return -1;
    struct ehci_hc *c = dev->hc;
    struct ehci_qh *qh = c->qh;
    struct ehci_qtd *qtd = c->qtd;
    uint8_t *xfer_buf = c->xfer_buf;
    uint8_t addr = dev->address;
    uint8_t max_packet = dev->max_packet0 ? dev->max_packet0 : 64;

    int dev_to_host = (bm_request_type & 0x80) != 0;

    /* stage the SETUP packet in DMA-visible memory */
    struct usb_setup *sp = (struct usb_setup *)(xfer_buf + EHCI_XFER_MAX);
    sp->bm_request_type = bm_request_type;
    sp->b_request = b_request;
    sp->w_value = w_value;
    sp->w_index = w_index;
    sp->w_length = w_length;
    uint32_t setup_phys = dma_phys(c, XFER_BUF_OFF) + EHCI_XFER_MAX;

    if (!dev_to_host && data && w_length)
        for (uint16_t i = 0; i < w_length; i++) xfer_buf[i] = ((const uint8_t *)data)[i];

    for (int i = 0; i < NUM_QTD; i++) qtd_clear(&qtd[i]);

    int n = 0;
    /* --- SETUP: always DATA0 --- */
    struct ehci_qtd *t_setup = &qtd[n++];
    t_setup->token = QTD_STS_ACTIVE | QTD_PID_SETUP | QTD_CERR_3 | QTD_LEN(8);
    qtd_buffers(t_setup, setup_phys, 8);

    /* --- DATA: first packet is DATA1 --- */
    struct ehci_qtd *t_data = 0;
    if (w_length) {
        t_data = &qtd[n++];
        t_data->token = QTD_STS_ACTIVE | QTD_CERR_3 | QTD_LEN(w_length) | QTD_TOGGLE
                      | (dev_to_host ? QTD_PID_IN : QTD_PID_OUT);
        qtd_buffers(t_data, dma_phys(c, XFER_BUF_OFF), w_length);
    }

    /* --- STATUS: zero length, DATA1, opposite direction to the data --- */
    struct ehci_qtd *t_status = &qtd[n++];
    t_status->token = QTD_STS_ACTIVE | QTD_CERR_3 | QTD_LEN(0) | QTD_TOGGLE | QTD_IOC
                    | (dev_to_host ? QTD_PID_OUT : QTD_PID_IN);

    /* chain them */
    for (int i = 0; i < n - 1; i++) qtd[i].next = qtd_phys(c, i + 1);
    qtd[n - 1].next = QTD_T;

    /* Point the queue head at the chain. The overlay must be cleared too: a
       stale token from the previous transfer would otherwise be re-executed. */
    qh->chars = QH_CHARS_ADDR(addr) | QH_CHARS_EP(0) | QH_CHARS_EPS_HIGH
              | QH_CHARS_DTC | QH_CHARS_HEAD | QH_CHARS_MPL(max_packet);
    qh->caps = QH_CAPS_MULT1;
    qh->current = 0;
    qh->token = 0;
    qh->alt_next = QTD_T;
    for (int i = 0; i < 5; i++) qh->buffer[i] = 0;
    qh->next = qtd_phys(c, 0);       /* set LAST: this is what starts the work */

    /* wait for the final qTD to retire */
    int err = 0;
    for (int ms = 0; ms < 1000; ms++) {
        uint32_t tok = t_status->token;
        if (!(tok & QTD_STS_ACTIVE)) {
            if (tok & QTD_STS_ERRMASK) err = 1;
            goto done;
        }
        /* A halt anywhere in the chain stops everything behind it, so the
           status qTD would stay active forever - check the QH overlay too. */
        if (qh->token & QTD_STS_HALTED) { err = 1; goto done; }
        timer_sleep(1);
    }
    err = 1;    /* timed out */

done:
    qh->next = QTD_T;
    if (err) return -1;

    int transferred = w_length;
    if (t_data) {
        /* the controller counts DOWN the remaining length as it goes */
        uint32_t remaining = (t_data->token >> 16) & 0x7FFF;
        transferred = (int)w_length - (int)remaining;
        if (transferred < 0) transferred = 0;
    }
    if (dev_to_host && data && transferred > 0)
        for (int i = 0; i < transferred; i++) ((uint8_t *)data)[i] = xfer_buf[i];

    return transferred;
}

/* ---- bulk transfers -----------------------------------------------------
 * Same queue head and qTDs as a control transfer, with two differences that
 * matter.
 *
 * First, there is no SETUP or STATUS stage - a bulk transfer is just data.
 *
 * Second, and much easier to get wrong: the data toggle PERSISTS across
 * transfers on a bulk endpoint. Control transfers reset it every time (SETUP is
 * always DATA0), but bulk carries on from wherever the last transfer left off,
 * and a device that receives the wrong toggle silently discards the packet and
 * retries forever. So bulk runs with DTC clear, which tells the controller to
 * keep the toggle in the queue head and advance it itself, and the value is
 * saved and restored around each transfer because this driver reuses one queue
 * head for every endpoint.
 *
 * `ep` is the endpoint ADDRESS from the descriptor: bit 7 is the direction. */
static uint8_t *toggle_slot(struct ehci_hc *c, uint8_t addr, uint8_t ep) {
    for (int i = 0; i < MAX_TOGGLES; i++)
        if (c->toggles[i].addr == addr && c->toggles[i].ep == ep) return &c->toggles[i].toggle;
    for (int i = 0; i < MAX_TOGGLES; i++)
        if (c->toggles[i].addr == 0) {
            c->toggles[i].addr = addr; c->toggles[i].ep = ep; c->toggles[i].toggle = 0;
            return &c->toggles[i].toggle;
        }
    return 0;
}

static int ehci_bulk(struct usb_device *dev, uint8_t ep, void *data, uint32_t len) {
    if (!dev || !dev->hc || !data) return -1;
    if (len == 0 || len > EHCI_XFER_MAX) return -1;
    struct ehci_hc *c = dev->hc;
    struct ehci_qh *qh = c->qh;
    uint8_t *xfer_buf = c->xfer_buf;

    int in = (ep & 0x80) != 0;
    uint16_t mps = in ? dev->ep_in_mps : dev->ep_out_mps;
    if (mps == 0) mps = 512;

    uint8_t *tg = toggle_slot(c, dev->address, ep);
    if (!tg) return -1;

    if (!in) for (uint32_t i = 0; i < len; i++) xfer_buf[i] = ((const uint8_t *)data)[i];

    for (int i = 0; i < NUM_QTD; i++) qtd_clear(&c->qtd[i]);
    struct ehci_qtd *t = &c->qtd[0];
    t->token = QTD_STS_ACTIVE | QTD_CERR_3 | QTD_LEN(len) | QTD_IOC
             | (in ? QTD_PID_IN : QTD_PID_OUT);
    qtd_buffers(t, dma_phys(c, XFER_BUF_OFF), (uint16_t)len);
    t->next = QTD_T;

    /* DTC clear: the queue head owns the toggle. Restore ours into the overlay
       before handing the work over. */
    qh->chars = QH_CHARS_ADDR(dev->address) | QH_CHARS_EP(ep & 0x0F)
              | QH_CHARS_EPS_HIGH | QH_CHARS_HEAD | QH_CHARS_MPL(mps);
    qh->caps = QH_CAPS_MULT1;
    qh->current = 0;
    qh->alt_next = QTD_T;
    qh->token = *tg ? QTD_TOGGLE : 0;
    for (int i = 0; i < 5; i++) qh->buffer[i] = 0;
    qh->next = qtd_phys(c, 0);

    int err = 0;
    for (int ms = 0; ms < 2000; ms++) {
        uint32_t tok = t->token;
        if (!(tok & QTD_STS_ACTIVE)) { if (tok & QTD_STS_ERRMASK) err = 1; goto done;
        }
        if (qh->token & QTD_STS_HALTED) { err = 1; goto done; }
        timer_sleep(1);
    }
    err = 1;

done:
    /* Save the toggle the controller advanced to, whatever happened - a partial
       transfer still moved it, and resetting it here would desynchronise the
       endpoint for every transfer after. */
    *tg = (qh->token & QTD_TOGGLE) ? 1 : 0;
    qh->next = QTD_T;
    if (err) return -1;

    uint32_t remaining = (t->token >> 16) & 0x7FFF;
    int transferred = (int)len - (int)remaining;
    if (transferred < 0) transferred = 0;

    if (in && transferred > 0)
        for (int i = 0; i < transferred; i++) ((uint8_t *)data)[i] = xfer_buf[i];
    return transferred;
}

/* Take an abandoned transfer off the async schedule and reset the software
   toggle (usb.h). Control and bulk share the one queue head, so whatever was
   left on it - a qTD a panic interrupted, or one a timeout gave up on - would
   otherwise run when the schedule next reaches it, ahead of the next request.
   The overlay is only written with the async schedule stopped, as the spec
   requires; it is restarted afterwards. */
static void ehci_reset_ep(struct usb_device *dev, uint8_t ep) {
    if (!dev || !dev->hc) return;
    struct ehci_hc *c = dev->hc;
    struct ehci_qh *qh = c->qh;
    uint32_t cmd = op_rd(c, OP_USBCMD);
    op_wr(c, OP_USBCMD, cmd & ~CMD_ASE);
    for (int i = 0; i < 20 && (op_rd(c, OP_USBSTS) & STS_ASS); i++) timer_sleep(1);

    for (int i = 0; i < NUM_QTD; i++) qtd_clear(&c->qtd[i]);
    qh->next = QTD_T;
    qh->alt_next = QTD_T;
    qh->current = 0;
    qh->token = 0;                       /* not active, not halted, DATA0 */

    op_wr(c, OP_USBCMD, cmd | CMD_ASE);
    for (int i = 0; i < 20 && !(op_rd(c, OP_USBSTS) & STS_ASS); i++) timer_sleep(1);

    if (ep & 0x0F) {                     /* the control endpoint's toggle is fixed */
        uint8_t *tg = toggle_slot(c, dev->address, ep);
        if (tg) *tg = 0;
    }
}

/* ---- interrupt transfers ------------------------------------------------
 * These live on the PERIODIC schedule, not the async ring: the controller walks
 * the 1024-entry frame list once per millisecond. Pointing every entry at one
 * queue head polls it every frame, which is the simplest correct arrangement.
 *
 * The transfer is armed once and left running, and each poll only asks whether
 * it finished. An idle keyboard NAKs rather than answering, so a poll that
 * waited would stall for its whole timeout on every check for a keypress. */
static int int_slot_for(struct ehci_hc *c, uint8_t addr, uint8_t ep) {
    for (int i = 0; i < MAX_INT_EPS; i++)
        if (c->int_eps[i].armed && c->int_eps[i].addr == addr && c->int_eps[i].ep == ep) return i;
    for (int i = 0; i < MAX_INT_EPS; i++)
        if (!c->int_eps[i].armed) { c->int_eps[i].addr = addr; c->int_eps[i].ep = ep; return i; }
    return -1;
}

static void arm_int(struct ehci_hc *c, struct usb_device *dev, int i, uint8_t ep, uint16_t len) {
    struct ehci_qh  *iqh = dma_virt(c, INT_QH_OFF(i));
    struct ehci_qtd *itd = dma_virt(c, INT_QTD_OFF(i));
    uint32_t buf = dma_phys(c, INT_BUF_OFF(i));

    itd->next = QTD_T;
    itd->alt_next = QTD_T;
    itd->token = QTD_STS_ACTIVE | QTD_CERR_3 | QTD_LEN(len) | QTD_PID_IN;
    for (int b = 0; b < 5; b++) itd->buffer[b] = 0;
    itd->buffer[0] = buf;
    itd->buffer[1] = (buf & ~0xFFFu) + 0x1000;

    uint16_t mps = dev->ep_int_mps ? dev->ep_int_mps : 8;
    uint32_t toggle = iqh->token & QTD_TOGGLE;     /* the controller owns it */
    iqh->chars = QH_CHARS_ADDR(dev->address) | QH_CHARS_EP(ep & 0x0F)
               | QH_CHARS_EPS_HIGH | QH_CHARS_MPL(mps);
    /* S-mask in the low byte: which microframes this endpoint may run in.
       0x01 is microframe 0, once per frame. Leaving it ZERO schedules the
       endpoint in no microframe at all and it silently never runs. */
    iqh->caps = QH_CAPS_MULT1 | 0x01;
    iqh->current = 0;
    iqh->alt_next = QTD_T;
    iqh->token = toggle;
    for (int b = 0; b < 5; b++) iqh->buffer[b] = 0;
    iqh->next = dma_phys(c, INT_QTD_OFF(i));

    c->int_eps[i].len = len;
    c->int_eps[i].armed = 1;
}

static int ehci_interrupt_poll(struct usb_device *dev, uint8_t ep,
                               void *data, uint32_t len) {
    if (!dev || !dev->hc || !data) return -1;
    if (len == 0 || len > 64) return -1;
    struct ehci_hc *c = dev->hc;

    int i = int_slot_for(c, dev->address, ep);
    if (i < 0) return -1;

    struct ehci_qtd *itd = dma_virt(c, INT_QTD_OFF(i));

    if (!c->int_list_live) {
        /* Chain the interrupt queue heads together and point every frame-list
           entry at the head. Until PSE is set the controller never reads the
           frame list at all. */
        for (int k = 0; k < MAX_INT_EPS; k++) {
            struct ehci_qh *q = dma_virt(c, INT_QH_OFF(k));
            q->chars = 0;
            q->caps = QH_CAPS_MULT1 | 0x01;
            q->current = 0;
            q->next = QTD_T;
            q->alt_next = QTD_T;
            q->token = 0;
            q->horizontal = (k + 1 < MAX_INT_EPS)
                          ? (dma_phys(c, INT_QH_OFF(k + 1)) | LINK_TYP_QH) : QTD_T;
        }
        volatile uint32_t *pflist = dma_virt(c, PFLIST_OFF);
        for (int k = 0; k < 1024; k++) pflist[k] = dma_phys(c, INT_QH_OFF(0)) | LINK_TYP_QH;
        op_wr(c, OP_USBCMD, op_rd(c, OP_USBCMD) | CMD_PSE);
        c->int_list_live = 1;
    }

    if (!c->int_eps[i].armed) { arm_int(c, dev, i, ep, (uint16_t)len); return 0; }

    uint32_t tok = itd->token;
    if (tok & QTD_STS_ACTIVE) return 0;             /* still waiting */
    if (tok & QTD_STS_ERRMASK) { arm_int(c, dev, i, ep, (uint16_t)len); return -1; }

    uint32_t remaining = (tok >> 16) & 0x7FFF;
    int got = (int)c->int_eps[i].len - (int)remaining;
    if (got < 0) got = 0;
    if (got > (int)len) got = (int)len;

    const uint8_t *src = dma_virt(c, INT_BUF_OFF(i));
    for (int b = 0; b < got; b++) ((uint8_t *)data)[b] = src[b];

    arm_int(c, dev, i, ep, (uint16_t)len);
    return got;
}

/* ---- addressing ---------------------------------------------------------
 * On EHCI the driver issues SET_ADDRESS itself, which means talking to the
 * device on address 0 first. Every device answers there with at least 8 bytes
 * of its device descriptor, and byte 7 is bMaxPacketSize0 - which has to be
 * known before asking for anything longer, because a wrong packet size makes
 * the transfer fail rather than merely run slowly.
 *
 * xHCI inverts all of this; see the note in usb.h. */
static int ehci_attach(struct usb_device *dev) {
    struct ehci_hc *c = dev->hc;
    struct usb_device_descriptor dd;
    for (unsigned i = 0; i < sizeof(dd); i++) ((uint8_t *)&dd)[i] = 0;

    dev->address = 0;
    dev->max_packet0 = 64;          /* the safe guess for the first 8 bytes */
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &dd, 8) < 8) return 0;

    dev->max_packet0 = dd.b_max_packet_size0 ? dd.b_max_packet_size0 : 64;

    uint8_t addr = c->next_address++;
    if (ehci_control(dev, 0x00, USB_REQ_SET_ADDRESS, addr, 0, 0, 0) < 0) return 0;
    dev->address = addr;
    timer_sleep(2);                 /* the spec's 2 ms address-recovery delay */
    return 1;
}

static const struct usb_hc_ops ehci_ops = {
    .name    = "EHCI",
    .attach  = ehci_attach,
    .control = ehci_control,
    .bulk    = ehci_bulk,
    .interrupt_poll = ehci_interrupt_poll,
    .reset_ep = ehci_reset_ep,
};

/* Wait for every port's connection to hold still for 100 ms (Linux's
 * HUB_DEBOUNCE_STABLE; Haiku's _DebouncePort), at most 1.5 s. Once
 * CONFIGFLAG hands the ports over from the companions, a device that was
 * attached to one reconnects here, and a single look at a fixed time -
 * which this was - reads a stick still reconnecting as an empty port. */
#define DEBOUNCE_STEP_MS    10
#define DEBOUNCE_STABLE_MS  100
#define DEBOUNCE_MAX_MS     1500
static void settle_ports(struct ehci_hc *c) {
    uint32_t last[EHCI_MAX_PORTS];
    uint32_t stable[EHCI_MAX_PORTS];
    unsigned n = c->nports < EHCI_MAX_PORTS ? c->nports : EHCI_MAX_PORTS;
    for (unsigned p = 0; p < n; p++) { last[p] = portsc_rd(c, p) & PORTSC_CCS; stable[p] = 0; }

    for (uint32_t t = 0; t < DEBOUNCE_MAX_MS; t += DEBOUNCE_STEP_MS) {
        timer_sleep(DEBOUNCE_STEP_MS);
        int settled = 1;
        for (unsigned p = 0; p < n; p++) {
            uint32_t sc = portsc_rd(c, p);
            if (sc & PORTSC_CSC)            /* write 1 to clear the change */
                op_wr(c, OP_PORTSC + p * 4, (sc & ~PORTSC_RWC_MASK) | PORTSC_CSC);
            uint32_t now = sc & PORTSC_CCS;
            if (now != last[p] || (sc & PORTSC_CSC)) { last[p] = now; stable[p] = 0; }
            else if (stable[p] < DEBOUNCE_STABLE_MS) stable[p] += DEBOUNCE_STEP_MS;
            if (stable[p] < DEBOUNCE_STABLE_MS) settled = 0;
        }
        if (settled) return;
    }
    klog("EHCI", SEV_WARN, "a port's connection never settled; scanning anyway");
}

/* Reset a port: hold for 50 ms (the spec's TDRSTR), then clear and let the
   controller finish. PORTSC_PED must be written 0 during a reset. 1 when the
   controller finished it. */
static int reset_port(struct ehci_hc *c, unsigned p) {
    uint32_t sc = portsc_rd(c, p);
    portsc_wr(c, p, (sc & ~PORTSC_PED) | PORTSC_RESET);
    timer_sleep(50);
    sc = portsc_rd(c, p);
    portsc_wr(c, p, sc & ~PORTSC_RESET);

    /* The controller clears the reset bit itself; the spec allows 2 ms. */
    for (int i = 0; i < 20; i++) {
        if (!(portsc_rd(c, p) & PORTSC_RESET)) return 1;
        timer_sleep(1);
    }
    return 0;
}

/* A device that does not answer its first request is reset and asked again,
   as Linux (PORT_INIT_TRIES) and Haiku (two tries) both do: a stick that
   was busy, or had not finished reconnecting, answers the second time. */
#define PORT_TRIES          3
#define RESET_RECOVERY_MS   50          /* TRSTRCY is 10 ms; Haiku waits 50 */

static void enumerate_port(struct ehci_hc *c, unsigned p) {
    uint32_t sc = portsc_rd(c, p);
    if (!(sc & PORTSC_CCS)) return;           /* nothing plugged in */
    c->seen |= (uint16_t)(1u << p);           /* tried once, whatever comes of it */

    /* A K-state line reading identifies a low-speed device before we even try:
       the spec says release it to the companion rather than attempt a reset. */
    if ((sc & PORTSC_LINESTS) == PORTSC_LS_KSTATE) {
        klog_u32("EHCI", SEV_INFO, "port ", p, LOG_COLOR_VALUE,
                 ": low-speed device, released to the companion controller");
        released++;
        portsc_wr(c, p, sc | PORTSC_OWNER);
        return;
    }

    for (int attempt = 0; attempt < PORT_TRIES; attempt++) {
        if (attempt) {
            klog_u32("EHCI", SEV_WARN, "port ", p, LOG_COLOR_VALUE, ": no answer, resetting again");
            timer_sleep(100);
            if (!(portsc_rd(c, p) & PORTSC_CCS)) {
                klog_u32("EHCI", SEV_WARN, "port ", p, LOG_COLOR_VALUE, ": device went away");
                return;
            }
        }
        if (!reset_port(c, p)) {
            klog_u32("EHCI", SEV_WARN, "port ", p, LOG_COLOR_VALUE, ": reset never completed");
            continue;
        }

        /* THE speed test. The controller enables the port only for a
           high-speed device; anything else must be handed to the companion or
           it becomes unreachable by everyone - which is how an EHCI driver
           kills a USB keyboard. */
        sc = portsc_rd(c, p);
        if (!(sc & PORTSC_PED)) {
            klog_u32("EHCI", SEV_INFO, "port ", p, LOG_COLOR_VALUE,
                     ": full-speed device, released to the companion controller");
            released++;
            portsc_wr(c, p, sc | PORTSC_OWNER);
            return;
        }

        /* Reset recovery: a device need not answer until TRSTRCY after the
           reset ends. The first request went out at once before. */
        timer_sleep(RESET_RECOVERY_MS);

        /* High speed confirmed. Everything from here is the core's job. */
        struct usb_device *dev = usb_alloc_device(&ehci_ops, c, (uint8_t)p, USB_SPEED_HIGH);
        if (!dev) {
            klog("EHCI", SEV_WARN, "device table full");
            return;
        }
        if (usb_enumerate(dev)) return;       /* it frees the device on failure */
    }
    klog_u32("EHCI", SEV_WARN, "port ", p, LOG_COLOR_VALUE, ": gave up on the device");
}

/* Bring up one controller. Returns 1 if it came up. */
static int ehci_start(struct ehci_hc *c, const struct pci_device *dev, unsigned index) {
    pci_enable_bus_master(dev);

    uint32_t base = pci_bar_mmio32(dev, 0, "EHCI");
    if (base == 0) return 0;
    for (uint32_t off = 0; off < 0x1000; off += 0x1000)
        paging_map_kernel(base + off, base + off, VM_WRITE | VM_UNCACHED);
    if (iowin_mmio(&c->cap_regs, (volatile void *)base, EHCI_BAR_SIZE) != 0) return 0;

    uint32_t hccparams = cap_rd32(c, CAP_HCCPARAMS);
    ehci_bios_handoff(dev, hccparams);

    uint8_t caplen = cap_rd8(c, CAP_CAPLENGTH);
    if (iowin_mmio(&c->op_regs, (volatile void *)(base + caplen), EHCI_BAR_SIZE - caplen) != 0)
        return 0;

    uint32_t hcsparams = cap_rd32(c, CAP_HCSPARAMS);
    c->nports = HCSPARAMS_NPORTS(hcsparams);
    c->port_power_control = HCSPARAMS_PPC(hcsparams);
    if (c->nports == 0) return 0;

    c->dma_v = usb_dma_alloc(EHCI_DMA_PAGES, EHCI_DMA_VBASE + index * 0x10000u, &c->dma);
    if (!c->dma_v) {
        klog("EHCI", SEV_WARN, "no memory for the controller's DMA pages");
        return 0;
    }
    c->qh  = dma_virt(c, QH_OFF);
    c->qtd = dma_virt(c, QTD_OFF);
    c->xfer_buf = dma_virt(c, XFER_BUF_OFF);

    /* Stop, then reset. Resetting a running controller is undefined. */
    op_wr(c, OP_USBCMD, op_rd(c, OP_USBCMD) & ~CMD_RS);
    for (int i = 0; i < 100 && !(op_rd(c, OP_USBSTS) & STS_HCHALTED); i++) timer_sleep(1);

    op_wr(c, OP_USBCMD, CMD_HCRESET);
    int reset_ok = 0;
    for (int i = 0; i < 250; i++) {
        if (!(op_rd(c, OP_USBCMD) & CMD_HCRESET)) { reset_ok = 1; break; }
        timer_sleep(1);
    }
    if (!reset_ok) {
        klog("EHCI", SEV_WARN, "controller reset did not complete");
        return 0;
    }

    /* The periodic list must exist and be valid even though nothing is
       scheduled on it - the controller reads it whenever PSE is set, and we
       leave PSE off. Every entry terminated. */
    volatile uint32_t *pflist = dma_virt(c, PFLIST_OFF);
    for (int i = 0; i < 1024; i++) pflist[i] = QTD_T;

    /* A single queue head serving as both the list head and the working QH:
       it links to itself, which is what makes it a ring. */
    struct ehci_qh *qh = c->qh;
    qh->horizontal = dma_phys(c, QH_OFF) | LINK_TYP_QH;
    qh->chars = QH_CHARS_HEAD;
    qh->caps = QH_CAPS_MULT1;
    qh->current = 0;
    qh->next = QTD_T;
    qh->alt_next = QTD_T;
    qh->token = 0;
    for (int i = 0; i < 5; i++) qh->buffer[i] = 0;

    op_wr(c, OP_CTRLDSSEG, 0);           /* 32-bit: everything is in segment 0 */
    op_wr(c, OP_PERIODICLIST, dma_phys(c, PFLIST_OFF));
    op_wr(c, OP_ASYNCLIST, dma_phys(c, QH_OFF));
    op_wr(c, OP_USBINTR, 0);             /* we poll */

    op_wr(c, OP_USBCMD, CMD_ASE | CMD_RS);
    for (int i = 0; i < 100 && (op_rd(c, OP_USBSTS) & STS_HCHALTED); i++) timer_sleep(1);
    if (op_rd(c, OP_USBSTS) & STS_HCHALTED) {
        klog("EHCI", SEV_WARN, "controller would not leave the halted state");
        return 0;
    }

    /* Route the ports to us rather than to the companion controllers. Until
       this is written the root ports answer to OHCI/UHCI and EHCI sees nothing. */
    op_wr(c, OP_CONFIGFLAG, 1);
    timer_sleep(5);

    klog_u32("EHCI", SEV_OK, "controller online, root ports: ", c->nports, LOG_COLOR_VALUE, "");
    klog_child_u32("  pci device ", dev->slot, LOG_COLOR_VALUE, "");

    for (unsigned p = 0; p < c->nports; p++) {
        if (c->port_power_control) {
            portsc_wr(c, p, portsc_rd(c, p) | PORTSC_POWER);
            timer_sleep(20);             /* the spec's settle time after power-on */
        }
    }
    settle_ports(c);                     /* let devices finish attaching */

    /* Said even when it is none, so a boot that finds no stick shows whether
       the controller saw it connected at all. */
    uint32_t connected = 0;
    for (unsigned p = 0; p < c->nports; p++)
        if (portsc_rd(c, p) & PORTSC_CCS) connected++;
    klog_child_u32("  ports with a device: ", connected, LOG_COLOR_VALUE, "");

    for (unsigned p = 0; p < c->nports; p++) enumerate_port(c, p);
    return 1;
}

void ehci_poll_ports(void) {
    for (unsigned h = 0; h < nhcs; h++) {
        struct ehci_hc *c = &hcs[h];
        unsigned n = c->nports < EHCI_MAX_PORTS ? c->nports : EHCI_MAX_PORTS;
        for (unsigned p = 0; p < n; p++) {
            uint32_t sc = portsc_rd(c, p);
            if (sc & PORTSC_OWNER) continue;               /* the companion's for now */
            if (sc & PORTSC_CSC)                           /* write 1 to clear the change */
                op_wr(c, OP_PORTSC + p * 4, (sc & ~PORTSC_RWC_MASK) | PORTSC_CSC);
            uint16_t bit = (uint16_t)(1u << p);
            if ((sc & PORTSC_CCS) && !(c->seen & bit)) {
                timer_sleep(100);                          /* connect debounce (USB 2.0 7.1.7.3) */
                enumerate_port(c, p);
            } else if (!(sc & PORTSC_CCS) && (c->seen & bit)) {
                c->seen &= (uint16_t)~bit;
                struct usb_device *d = usb_find(c, (uint8_t)p);
                if (d) usb_port_gone(d);
            }
        }
    }
}

int ehci_init(void) {
    nhcs = 0;
    /* class 0x0C serial bus, subclass 0x03 USB, prog-if 0x20 = EHCI. Every
       one of them: each has its own ports. */
    for (unsigned k = 0; nhcs < EHCI_MAX_HCS; k++) {
        const struct pci_device *dev = pci_find(0x0C, 0x03, 0x20, k);
        if (!dev) break;
        struct ehci_hc *c = &hcs[nhcs];
        for (unsigned i = 0; i < sizeof(*c); i++) ((uint8_t *)c)[i] = 0;
        c->next_address = 1;
        if (ehci_start(c, dev, nhcs)) nhcs++;
    }
    return (int)nhcs;
}
