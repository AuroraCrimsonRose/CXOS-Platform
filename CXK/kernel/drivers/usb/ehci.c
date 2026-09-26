/* /CXK/kernel/drivers/usb/ehci.c */
/* Aurora Tejeda / CATX Systems LLC */
/* EHCI host controller, stage 1. See ehci.h for scope and for why a USB
   keyboard is deliberately not this driver's job. */

#include "ehci.h"
#include "usb.h"
#include "pci.h"
#include "paging.h"
#include "logging.h"
#include "timer.h"

/* ---- capability registers (at BAR0) ------------------------------------- */
#define CAP_CAPLENGTH   0x00   /* 8-bit: distance from BAR0 to the op regs   */
#define CAP_HCSPARAMS   0x04
#define CAP_HCCPARAMS   0x08

#define HCSPARAMS_NPORTS(x)  ((x) & 0x0F)
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

/* ---- DMA window ---------------------------------------------------------
 * Same pattern as the NIC drivers: the controller bus-masters from PHYSICAL
 * addresses while the CPU reaches the identical structures through a KERNEL-half
 * alias, so they stay visible when CR3 points at a process directory.
 *
 * 0x760000 is inside the 0x500000-0x800000 window pmm.c already reserves for
 * driver DMA, past the RTL8169 region which ends at 0x752000. */
#define EHCI_DMA_BASE   0x760000u
#define EHCI_DMA_VBASE  0xE3000000u
#define DMA_V(phys)     ((phys) - EHCI_DMA_BASE + EHCI_DMA_VBASE)

#define PFLIST_ADDR     (EHCI_DMA_BASE + 0x0000)   /* 1024 dwords, 4K aligned */
#define QH_ADDR         (EHCI_DMA_BASE + 0x1000)
#define QTD_ADDR        (EHCI_DMA_BASE + 0x2000)
#define XFER_BUF_ADDR   (EHCI_DMA_BASE + 0x3000)   /* EHCI_XFER_MAX, page aligned */
#define EHCI_DMA_END    (EHCI_DMA_BASE + 0x8000)

#define NUM_QTD 4

static volatile uint8_t *cap_regs = 0;
static volatile uint8_t *op_regs = 0;
static int present = 0;
static unsigned nports = 0;
static int port_power_control = 0;

static uint8_t next_address = 1;

static inline uint32_t cap_rd32(uint32_t o) { return *(volatile uint32_t *)(cap_regs + o); }
static inline uint8_t  cap_rd8(uint32_t o)  { return *(volatile uint8_t  *)(cap_regs + o); }
static inline uint32_t op_rd(uint32_t o)    { return *(volatile uint32_t *)(op_regs + o); }
static inline void     op_wr(uint32_t o, uint32_t v) { *(volatile uint32_t *)(op_regs + o) = v; }

static inline uint32_t portsc_rd(unsigned p) { return op_rd(OP_PORTSC + p * 4); }

/* Write PORTSC without disturbing the write-1-to-clear change bits. Every port
   write in this driver goes through here for that reason. */
static inline void portsc_wr(unsigned p, uint32_t v) {
    op_wr(OP_PORTSC + p * 4, v & ~PORTSC_RWC_MASK);
}

static struct ehci_qh  *qh  = 0;
static struct ehci_qtd *qtd = 0;
static uint8_t *xfer_buf = 0;

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

    for (int i = 0; i < 250; i++) {        /* up to ~250 ms, per the spec's guidance */
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
static uint32_t qtd_phys(int i) {
    return QTD_ADDR + (uint32_t)i * (uint32_t)sizeof(struct ehci_qtd);
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
    if (!present || !dev) return -1;
    if (w_length > EHCI_XFER_MAX) return -1;
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
    uint32_t setup_phys = XFER_BUF_ADDR + EHCI_XFER_MAX;

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
        qtd_buffers(t_data, XFER_BUF_ADDR, w_length);
    }

    /* --- STATUS: zero length, DATA1, opposite direction to the data --- */
    struct ehci_qtd *t_status = &qtd[n++];
    t_status->token = QTD_STS_ACTIVE | QTD_CERR_3 | QTD_LEN(0) | QTD_TOGGLE | QTD_IOC
                    | (dev_to_host ? QTD_PID_OUT : QTD_PID_IN);

    /* chain them */
    for (int i = 0; i < n - 1; i++) qtd[i].next = qtd_phys(i + 1);
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
    qh->next = qtd_phys(0);          /* set LAST: this is what starts the work */

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
#define MAX_TOGGLES 16
static struct { uint8_t addr, ep, toggle; } toggles[MAX_TOGGLES];

static uint8_t *toggle_slot(uint8_t addr, uint8_t ep) {
    for (int i = 0; i < MAX_TOGGLES; i++)
        if (toggles[i].addr == addr && toggles[i].ep == ep) return &toggles[i].toggle;
    for (int i = 0; i < MAX_TOGGLES; i++)
        if (toggles[i].addr == 0) {
            toggles[i].addr = addr; toggles[i].ep = ep; toggles[i].toggle = 0;
            return &toggles[i].toggle;
        }
    return 0;
}

static int ehci_bulk(struct usb_device *dev, uint8_t ep, void *data, uint32_t len) {
    if (!present || !dev || !data) return -1;
    if (len == 0 || len > EHCI_XFER_MAX) return -1;

    int in = (ep & 0x80) != 0;
    uint16_t mps = in ? dev->ep_in_mps : dev->ep_out_mps;
    if (mps == 0) mps = 512;

    uint8_t *tg = toggle_slot(dev->address, ep);
    if (!tg) return -1;

    if (!in) for (uint32_t i = 0; i < len; i++) xfer_buf[i] = ((const uint8_t *)data)[i];

    for (int i = 0; i < NUM_QTD; i++) qtd_clear(&qtd[i]);
    struct ehci_qtd *t = &qtd[0];
    t->token = QTD_STS_ACTIVE | QTD_CERR_3 | QTD_LEN(len) | QTD_IOC
             | (in ? QTD_PID_IN : QTD_PID_OUT);
    qtd_buffers(t, XFER_BUF_ADDR, (uint16_t)len);
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
    qh->next = qtd_phys(0);

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

/* Reset the software toggle for an endpoint. The mass-storage layer calls this
   after clearing a stall, because CLEAR_FEATURE(ENDPOINT_HALT) resets the
   toggle on the DEVICE side and the two must agree. */
static void ehci_reset_toggle(struct usb_device *dev, uint8_t ep) {
    uint8_t *tg = toggle_slot(dev->address, ep);
    if (tg) *tg = 0;
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
    struct usb_device_descriptor dd;
    for (unsigned i = 0; i < sizeof(dd); i++) ((uint8_t *)&dd)[i] = 0;

    dev->address = 0;
    dev->max_packet0 = 64;          /* the safe guess for the first 8 bytes */
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &dd, 8) < 8) return 0;

    dev->max_packet0 = dd.b_max_packet_size0 ? dd.b_max_packet_size0 : 64;

    uint8_t addr = next_address++;
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
    .reset_toggle = ehci_reset_toggle,
};

static void enumerate_port(unsigned p) {
    uint32_t sc = portsc_rd(p);
    if (!(sc & PORTSC_CCS)) return;           /* nothing plugged in */

    /* A K-state line reading identifies a low-speed device before we even try:
       the spec says release it to the companion rather than attempt a reset. */
    if ((sc & PORTSC_LINESTS) == PORTSC_LS_KSTATE) {
        klog_u32("EHCI", SEV_INFO, "port ", p, LOG_COLOR_VALUE,
                 ": low-speed device, released to the companion controller");
        portsc_wr(p, sc | PORTSC_OWNER);
        return;
    }

    /* Reset: hold for 50 ms (the spec's TDRSTR), then clear and let the
       controller finish. PORTSC_PED must be written 0 during a reset. */
    portsc_wr(p, (sc & ~PORTSC_PED) | PORTSC_RESET);
    timer_sleep(50);
    sc = portsc_rd(p);
    portsc_wr(p, sc & ~PORTSC_RESET);

    /* The controller clears the reset bit itself; the spec allows 2 ms. */
    int cleared = 0;
    for (int i = 0; i < 20; i++) {
        if (!(portsc_rd(p) & PORTSC_RESET)) { cleared = 1; break; }
        timer_sleep(1);
    }
    if (!cleared) {
        klog_u32("EHCI", SEV_WARN, "port ", p, LOG_COLOR_VALUE, ": reset never completed");
        return;
    }

    /* THE speed test. The controller enables the port only for a high-speed
       device; anything else must be handed to the companion or it becomes
       unreachable by everyone - which is how an EHCI driver kills a USB
       keyboard. */
    sc = portsc_rd(p);
    if (!(sc & PORTSC_PED)) {
        klog_u32("EHCI", SEV_INFO, "port ", p, LOG_COLOR_VALUE,
                 ": full-speed device, released to the companion controller");
        portsc_wr(p, sc | PORTSC_OWNER);
        return;
    }

    /* High speed confirmed. Everything from here is the core's job. */
    struct usb_device *dev = usb_alloc_device(&ehci_ops, 0, (uint8_t)p, USB_SPEED_HIGH);
    if (!dev) {
        klog("EHCI", SEV_WARN, "device table full");
        return;
    }
    usb_enumerate(dev);
}

int ehci_init(void) {
    present = 0;
    next_address = 1;

    /* class 0x0C serial bus, subclass 0x03 USB, prog-if 0x20 = EHCI */
    const struct pci_device *dev = pci_find(0x0C, 0x03, 0x20, 0);
    if (!dev) return 0;

    uint32_t base = dev->bar[0] & 0xFFFFFFF0u;
    if (base == 0) return 0;

    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);          /* memory space + bus master */
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    /* Identity-mapped registers, same rule as the NIC drivers: only safe
       because PCI MMIO lands in the shared kernel half. */
    if (base < 0xC0000000u) {
        klog_u32("EHCI", SEV_WARN, "register BAR below the kernel half: ", base,
                 LOG_COLOR_VALUE, " - not visible from a process address space");
        return 0;
    }
    for (uint32_t off = 0; off < 0x1000; off += 0x1000)
        paging_map(base + off, base + off, PAGE_PRESENT | PAGE_WRITE | PAGE_NO_CACHE);
    cap_regs = (volatile uint8_t *)base;

    uint32_t hccparams = cap_rd32(CAP_HCCPARAMS);
    ehci_bios_handoff(dev, hccparams);

    op_regs = cap_regs + cap_rd8(CAP_CAPLENGTH);

    uint32_t hcsparams = cap_rd32(CAP_HCSPARAMS);
    nports = HCSPARAMS_NPORTS(hcsparams);
    port_power_control = HCSPARAMS_PPC(hcsparams);
    if (nports == 0) return 0;

    /* DMA window into the kernel half, cacheable - x86 snoops bus-master DMA. */
    for (uint32_t a = EHCI_DMA_BASE; a < EHCI_DMA_END; a += 0x1000)
        paging_map(DMA_V(a), a, PAGE_PRESENT | PAGE_WRITE);

    qh  = (struct ehci_qh  *)DMA_V(QH_ADDR);
    qtd = (struct ehci_qtd *)DMA_V(QTD_ADDR);
    xfer_buf = (uint8_t *)DMA_V(XFER_BUF_ADDR);

    /* Stop, then reset. Resetting a running controller is undefined. */
    op_wr(OP_USBCMD, op_rd(OP_USBCMD) & ~CMD_RS);
    for (int i = 0; i < 100 && !(op_rd(OP_USBSTS) & STS_HCHALTED); i++) timer_sleep(1);

    op_wr(OP_USBCMD, CMD_HCRESET);
    int reset_ok = 0;
    for (int i = 0; i < 250; i++) {
        if (!(op_rd(OP_USBCMD) & CMD_HCRESET)) { reset_ok = 1; break; }
        timer_sleep(1);
    }
    if (!reset_ok) {
        klog("EHCI", SEV_WARN, "controller reset did not complete");
        return 0;
    }

    /* The periodic list must exist and be valid even though nothing is
       scheduled on it - the controller reads it whenever PSE is set, and we
       leave PSE off. Every entry terminated. */
    volatile uint32_t *pflist = (volatile uint32_t *)DMA_V(PFLIST_ADDR);
    for (int i = 0; i < 1024; i++) pflist[i] = QTD_T;

    /* A single queue head serving as both the list head and the working QH:
       it links to itself, which is what makes it a ring. */
    qh->horizontal = QH_ADDR | LINK_TYP_QH;
    qh->chars = QH_CHARS_HEAD;
    qh->caps = QH_CAPS_MULT1;
    qh->current = 0;
    qh->next = QTD_T;
    qh->alt_next = QTD_T;
    qh->token = 0;
    for (int i = 0; i < 5; i++) qh->buffer[i] = 0;

    op_wr(OP_CTRLDSSEG, 0);              /* 32-bit: everything is in segment 0 */
    op_wr(OP_PERIODICLIST, PFLIST_ADDR);
    op_wr(OP_ASYNCLIST, QH_ADDR);
    op_wr(OP_USBINTR, 0);                /* we poll */

    op_wr(OP_USBCMD, CMD_ASE | CMD_RS);
    for (int i = 0; i < 100 && (op_rd(OP_USBSTS) & STS_HCHALTED); i++) timer_sleep(1);
    if (op_rd(OP_USBSTS) & STS_HCHALTED) {
        klog("EHCI", SEV_WARN, "controller would not leave the halted state");
        return 0;
    }

    /* Route the ports to us rather than to the companion controllers. Until
       this is written the root ports answer to OHCI/UHCI and EHCI sees nothing. */
    op_wr(OP_CONFIGFLAG, 1);
    timer_sleep(5);

    present = 1;
    klog_u32("EHCI", SEV_OK, "controller online, root ports: ", nports, LOG_COLOR_VALUE, "");

    for (unsigned p = 0; p < nports; p++) {
        if (port_power_control) {
            portsc_wr(p, portsc_rd(p) | PORTSC_POWER);
            timer_sleep(20);             /* the spec's settle time after power-on */
        }
    }
    timer_sleep(100);                    /* let devices finish attaching */

    for (unsigned p = 0; p < nports; p++) enumerate_port(p);

    return 1;
}
