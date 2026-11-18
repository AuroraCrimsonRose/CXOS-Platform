// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/usb/usb_hid.c */
/* Aurora Tejeda / CATX Systems */
/* USB HID boot-protocol keyboard and mouse. See usb_hid.h. */

#include "usb_hid.h"
#include "usb.h"
#include "drivers/devtree.h"
#include "keyboard.h"
#include "mouse.h"
#include "logging.h"

/* HID class requests, on the interface (bmRequestType 0x21) */
#define HID_REQ_SET_PROTOCOL  0x0B
#define HID_REQ_SET_IDLE      0x0A
#define HID_PROTOCOL_BOOT     0
#define HID_PROTOCOL_REPORT   1
#define USB_REQ_GET_DESC      0x06
#define HID_DESC_REPORT       0x22

/* HID subclass 1 = "boot interface"; protocol 1 = keyboard, 2 = mouse. */
#define HID_SUBCLASS_BOOT     1
#define HID_PROTO_KEYBOARD    1
#define HID_PROTO_MOUSE       2

#define MAX_HID 4

struct hid_device {
    struct usb_device *dev;
    uint8_t in_use;
    uint8_t is_keyboard;
    uint8_t prev_keys[6];     /* for edge detection - see poll_keyboard */
    uint8_t prev_mods;
    uint8_t told;             /* the mouse's first report is in the log */
    /* A mouse: where its fields are, from its report descriptor. */
    uint8_t report_id;        /* 0: the device numbers no reports */
    uint8_t x_size, y_size, btn_count;
    uint16_t x_bit, y_bit, btn_bit;
};

static struct hid_device hids[MAX_HID];
static int any_present = 0;
static int keyboards = 0;

/* ---- the reports --------------------------------------------------------
 * Keyboard, 8 bytes: modifiers, reserved, then up to six usage ids of the keys
 * CURRENTLY HELD. It is a state snapshot, not an event - a key held down
 * reappears in every report. So a keystroke is the difference between this
 * report and the last one, which is what prev_keys is for. Treating each report
 * as an event would repeat every held key at the poll rate.
 *
 * Usage ids are what keyboard_key takes (2.8a2): the modifier byte's bit i is
 * usage 0xE0 + i. Modifiers going down come first and modifiers coming up
 * last, so a report that brings Ctrl and C together is Ctrl+C, and one that
 * lets go of Ctrl as C goes down is a plain c. */
static int in_report(const uint8_t *keys, uint8_t u) {
    for (int j = 0; j < 6; j++) if (keys[j] == u) return 1;
    return 0;
}

static void poll_keyboard(struct hid_device *h) {
    uint8_t rpt[8];
    for (int i = 0; i < 8; i++) rpt[i] = 0;

    struct usb_device *dev = h->dev;
    int n = dev->ops->interrupt_poll(dev, dev->ep_int_in, rpt, 8);
    if (n < 8) return;               /* nothing new, or a runt report */

    uint8_t mods = rpt[0];
    const uint8_t *keys = &rpt[2];
    /* 1 = rollover error: too many keys held to say which; nothing changes */
    for (int i = 0; i < 6; i++) if (keys[i] == 1) return;

    for (int b = 0; b < 8; b++)
        if ((mods & (1u << b)) && !(h->prev_mods & (1u << b))) keyboard_key(0xE0u + (uint32_t)b, 1);
    for (int i = 0; i < 6; i++)
        if (h->prev_keys[i] > 1 && !in_report(keys, h->prev_keys[i])) keyboard_key(h->prev_keys[i], 0);
    for (int i = 0; i < 6; i++)
        if (keys[i] > 1 && !in_report(h->prev_keys, keys[i])) keyboard_key(keys[i], 1);
    for (int b = 0; b < 8; b++)
        if (!(mods & (1u << b)) && (h->prev_mods & (1u << b))) keyboard_key(0xE0u + (uint32_t)b, 0);

    for (int i = 0; i < 6; i++) h->prev_keys[i] = keys[i];
    h->prev_mods = mods;
}

/* ---- the report descriptor ------------------------------------------------
 * A mouse says where its fields are in a small bytecode (HID 1.11, 6.2.2). Not
 * every mouse that claims the boot interface sends the boot report: the FX's
 * sent X and Y as 16 bits each, so the byte read as Y was the top of X, and
 * the pointer drifted up as it went left and took no vertical movement at all.
 *
 * Only what a pointer needs: the first report with relative X and Y, and its
 * buttons. Global and local state, an Input item at a time; Push, Pop and
 * delimiters are for layouts no mouse uses. */
#define HID_DESC_MAX     1024       /* a gaming mouse's runs to a few hundred */
#define HID_DESC_GUESS   256        /* no HID descriptor gave the length: Haiku's guess */
#define HID_USAGES_MAX   8
#define HID_PAGE_DESKTOP 0x01
#define HID_PAGE_BUTTON  0x09
/* Usages as the parser keeps them: page in the high 16 bits. A 4-byte
   Usage item says its own page ("extended", HID 1.11, 6.2.2.8); a shorter
   one takes the Usage Page in force at its Main item, as Haiku's
   HIDParser.cpp makes every usage extended there. */
#define HID_USAGE_X      0x00010030u
#define HID_USAGE_Y      0x00010031u
#define HID_USAGE_BTN1   0x00090001u
#define HID_IN_CONSTANT  0x01
#define HID_IN_VARIABLE  0x02
#define HID_IN_RELATIVE  0x04

static int parse_mouse_report(struct hid_device *h, const uint8_t *d, int n) {
    uint32_t page = 0, size = 0, count = 0, id = 0, bit = 0;
    uint32_t usage[HID_USAGES_MAX], nusage = 0, umin = 0, umax = 0;
    int range = 0, have_x = 0, have_y = 0, have_btn = 0;

    for (int p = 0; p < n; ) {
        uint8_t b = d[p++];
        if (b == 0xFE) {                    /* a long item: nothing we want */
            if (p >= n) break;
            p += 2 + d[p];
            continue;
        }
        int sz = b & 3;
        if (sz == 3) sz = 4;
        if (p + sz > n) break;
        uint32_t v = 0;
        for (int i = 0; i < sz; i++) v |= (uint32_t)d[p + i] << (8 * i);
        p += sz;

        int type = (b >> 2) & 3, tag = b >> 4;
        if (type == 1) {                    /* global */
            if (tag == 0) page = v;
            else if (tag == 7) size = v;
            else if (tag == 9) count = v;
            else if (tag == 8 && v != id) {
                if (have_x && have_y) break;    /* the pointer's report has ended */
                id = v;
                bit = 0;                    /* each report counts from its own start */
                have_x = have_y = have_btn = 0;
            }
        } else if (type == 2) {             /* local */
            /* sz 4: extended, the page in the high half; else the page comes later */
            uint32_t lu = sz == 4 ? v : (v & 0xFFFF);
            if (tag == 0 && nusage < HID_USAGES_MAX) usage[nusage++] = lu;
            else if (tag == 1) { umin = lu; range = 1; }
            else if (tag == 2) umax = lu;
        } else if (type == 0) {             /* main: it uses up the local state */
            if (tag == 8) {                 /* Input */
                if (count > 1024 || size > 32) return 0;
                uint32_t pg = (page & 0xFFFF) << 16;
                if (range && umin <= 0xFFFF) umin |= pg;
                if (range && umax <= 0xFFFF) umax |= pg;
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t u = range ? umin + i
                               : nusage ? usage[i < nusage ? i : nusage - 1] : 0;
                    if (range && u > umax) u = 0;
                    if (!range && u && u <= 0xFFFF) u |= pg;
                    int data = !(v & HID_IN_CONSTANT) && (v & HID_IN_VARIABLE);
                    if (data && (v & HID_IN_RELATIVE) && size) {
                        if (u == HID_USAGE_X && !have_x) { h->x_bit = (uint16_t)bit; h->x_size = (uint8_t)size; have_x = 1; }
                        if (u == HID_USAGE_Y && !have_y) { h->y_bit = (uint16_t)bit; h->y_size = (uint8_t)size; have_y = 1; }
                    }
                    if (data && size == 1 && u == HID_USAGE_BTN1 && !have_btn) {
                        h->btn_bit = (uint16_t)bit;
                        h->btn_count = (uint8_t)(count > 3 ? 3 : count);
                        have_btn = 1;
                    }
                    bit += size;
                }
            }
            nusage = 0; range = 0; umin = 0; umax = 0;
        }
    }
    if (!have_x || !have_y) return 0;
    if (!have_btn) h->btn_count = 0;
    h->report_id = (uint8_t)id;
    return 1;
}

/* A signed field. The caller has checked that it lies inside the report. */
static int32_t report_field(const uint8_t *r, uint32_t bit, uint32_t size) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; i++, bit++)
        if ((r[bit / 8] >> (bit % 8)) & 1) v |= 1u << i;
    if (size < 32 && (v >> (size - 1)) & 1) v |= ~0u << size;
    return (int32_t)v;
}

/* Mouse, in the boot format: buttons, then SIGNED X and Y deltas. Y is positive
   DOWNWARD, which already matches screen coordinates - no inversion.

   Three bytes are what we read, but the device may send up to a whole packet
   (a wheel, spare buttons, padding), so the buffer offered is a whole packet.
   A report longer than the buffer is an overrun, and on OHCI that halts the
   endpoint: every report was lost, and the pointer never moved at all. */
#define HID_REPORT_MAX 64           /* what interrupt_poll takes, on all three */

static void poll_mouse(struct hid_device *h) {
    uint8_t rpt[HID_REPORT_MAX];
    for (int i = 0; i < HID_REPORT_MAX; i++) rpt[i] = 0;

    struct usb_device *dev = h->dev;
    uint32_t want = dev->ep_int_mps & 0x7FFu;
    if (want == 0) want = 8;
    if (want > HID_REPORT_MAX) want = HID_REPORT_MAX;
    int n = dev->ops->interrupt_poll(dev, dev->ep_int_in, rpt, want);
    if (n < 3) return;

    if (!h->told) {                 /* what this mouse really sends, for a rig's log */
        h->told = 1;
        klog_u32("USBHID", SEV_INFO, "mouse's first report, bytes ", (uint32_t)n, LOG_COLOR_VALUE, "");
        klog_u32("USBHID", SEV_INFO, "-> its first four, low byte first ",
                 (uint32_t)rpt[0] | (uint32_t)rpt[1] << 8 | (uint32_t)rpt[2] << 16 | (uint32_t)rpt[3] << 24,
                 LOG_COLOR_VALUE, "");
    }

    const uint8_t *r = rpt;
    uint32_t bits = (uint32_t)n * 8;
    if (h->report_id) {
        if (rpt[0] != h->report_id) return;     /* some other report of its */
        r++;
        bits -= 8;
    }
    if (h->x_bit + h->x_size > bits || h->y_bit + h->y_size > bits) return;
    uint32_t btns = 0;
    for (uint32_t i = 0; i < h->btn_count && h->btn_bit + i < bits; i++)
        if ((r[(h->btn_bit + i) / 8] >> ((h->btn_bit + i) % 8)) & 1) btns |= 1u << i;
    mouse_inject(report_field(r, h->x_bit, h->x_size),
                 report_field(r, h->y_bit, h->y_size), btns);
}

/* Up to 64 bytes of a descriptor we could not use, 16 to a log line, so a
   rig's log says what the mouse declared. */
static void log_bytes(const uint8_t *d, int n) {
    static const char hex[] = "0123456789ABCDEF";
    if (n > 64) n = 64;
    for (int at = 0; at < n; at += 16) {
        char line[16 * 3 + 1];
        int k = 0;
        for (int i = at; i < n && i < at + 16; i++) {
            line[k++] = hex[d[i] >> 4];
            line[k++] = hex[d[i] & 15];
            line[k++] = ' ';
        }
        line[k] = 0;
        klog_child(line);
    }
}

/* A mouse is read only as its report descriptor says: Haiku and SerenityOS
   never put a mouse in the boot protocol. Many mice that claim the boot
   interface ignore SET_PROTOCOL(boot) and go on sending their own reports
   (the FX's 10C4:0005 sent six bytes), and three bytes read as the boot
   format then are the wrong fields. A descriptor we cannot read or use
   leaves the mouse unattached, with the reason and the bytes in the log. */
static int mouse_layout(struct hid_device *h, struct usb_device *dev) {
    static uint8_t desc[HID_DESC_MAX];
    uint32_t want = dev->hid_report_len ? dev->hid_report_len : HID_DESC_GUESS;
    if (want > HID_DESC_MAX) {
        klog_u32("USBHID", SEV_WARN, "mouse skipped: its report descriptor is too long, bytes ",
                 want, LOG_COLOR_VALUE, "");
        return 0;
    }
    int got = dev->ops->control(dev, 0x81, USB_REQ_GET_DESC, HID_DESC_REPORT << 8,
                                dev->if_number, desc, (uint16_t)want);
    if (got <= 0) {
        klog_u32("USBHID", SEV_WARN, "mouse skipped: its report descriptor was not read, asked for bytes ",
                 want, LOG_COLOR_VALUE, "");
        return 0;
    }
    if (dev->hid_report_len && (uint32_t)got < want)
        klog_u32("USBHID", SEV_WARN, "mouse's report descriptor came short, bytes ",
                 (uint32_t)got, LOG_COLOR_VALUE, "");
    if (!parse_mouse_report(h, desc, got)) {
        klog_u32("USBHID", SEV_WARN, "mouse skipped: no relative X and Y in its report descriptor, bytes ",
                 (uint32_t)got, LOG_COLOR_VALUE, "");
        log_bytes(desc, got);
        return 0;
    }
    return 1;
}

int usb_hid_attach(struct usb_device *dev) {
    if (!dev || !dev->ops || !dev->ops->interrupt_poll) return 0;
    if (dev->if_class != USB_CLASS_HID) return 0;
    if (dev->if_subclass != HID_SUBCLASS_BOOT) {
        /* Without the boot interface we would need a report-descriptor parser,
           which is a real piece of work and not this. */
        klog("USBHID", SEV_INFO, "HID device without a boot interface - skipped");
        return 0;
    }
    if (!dev->ep_int_in) {
        klog("USBHID", SEV_WARN, "HID device with no interrupt IN endpoint - skipped");
        return 0;
    }

    int kbd = dev->if_protocol == HID_PROTO_KEYBOARD;
    int mse = dev->if_protocol == HID_PROTO_MOUSE;
    if (!kbd && !mse) return 0;

    struct hid_device *h = 0;
    for (int i = 0; i < MAX_HID; i++) if (!hids[i].in_use) { h = &hids[i]; break; }
    if (!h) return 0;

    for (unsigned i = 0; i < sizeof(*h); i++) ((uint8_t *)h)[i] = 0;
    h->dev = dev;
    h->is_keyboard = (uint8_t)kbd;
    h->in_use = 1;

    if (mse && !mouse_layout(h, dev)) { h->in_use = 0; return 0; }

    /* SET_PROTOCOL. A device powers up in report mode, and a keyboard's reports
       then have whatever shape its descriptor declares, which we do not read;
       so it goes to boot. A mouse is set to report explicitly: firmware that
       drove it in boot may have left it there. wValue is the protocol, wIndex
       the interface. */
    int prc = dev->ops->control(dev, 0x21, HID_REQ_SET_PROTOCOL,
                                mse ? HID_PROTOCOL_REPORT : HID_PROTOCOL_BOOT,
                                dev->if_number, 0, 0);
    if (prc < 0) klog("USBHID", SEV_INFO, "SET_PROTOCOL refused; it is read as it reports anyway");

    /* SET_IDLE(0) - report only on change rather than re-sending forever. Some
       devices STALL this; it is an optimisation, not a requirement. */
    dev->ops->control(dev, 0x21, HID_REQ_SET_IDLE, 0, dev->if_number, 0, 0);

    any_present = 1;
    keyboard_usb_arrived();     /* the shell is usually asleep by now */
    keyboards += kbd;
    klog("USBHID", SEV_OK, kbd ? "keyboard online (boot protocol)"
                               : "mouse online (its own report layout)");
    if (mse) {
        klog_u32("USBHID", SEV_INFO, "-> report descriptor, bytes ", dev->hid_report_len, LOG_COLOR_VALUE, "");
        klog_u32("USBHID", SEV_INFO, "-> report id ", h->report_id, LOG_COLOR_VALUE, "");
        klog_u32("USBHID", SEV_INFO, "-> X at bit ", h->x_bit, LOG_COLOR_VALUE, "");
        klog_u32("USBHID", SEV_INFO, "-> Y at bit ", h->y_bit, LOG_COLOR_VALUE, "");
        klog_u32("USBHID", SEV_INFO, "-> bits in each ", h->x_size, LOG_COLOR_VALUE, "");
    }
    return 1;
}

int usb_hid_detach(struct usb_device *dev) {
    int found = 0, any = 0;
    for (int i = 0; i < MAX_HID; i++) {
        if (hids[i].in_use && hids[i].dev == dev) {
            keyboards -= hids[i].is_keyboard;
            hids[i].in_use = 0;
            found = 1;
        }
        if (hids[i].in_use) any = 1;
    }
    any_present = any;
    if (found) klog("USBHID", SEV_INFO, "device unplugged");
    return found;
}

void usb_hid_poll(void) {
    if (!any_present) return;
    int k = usb_lock();     /* the idle loop's port polling shares the controller */
    for (int i = 0; i < MAX_HID; i++) {
        if (!hids[i].in_use) continue;
        if (hids[i].is_keyboard) poll_keyboard(&hids[i]);
        else                     poll_mouse(&hids[i]);
    }
    usb_unlock(k);
}

int usb_hid_present(void) { return any_present; }
int usb_hid_keyboards(void) { return keyboards; }

/* ---- the device tree's view (drivers/devtree.h) ----
   Any HID interface; usb_hid_attach declines what it cannot drive (no boot
   interface, no interrupt endpoint), as it always did. */
static int hid_bind(struct dev_node *n) { return usb_hid_attach(n->usb); }
static void hid_remove(struct dev_node *n) { (void)usb_hid_detach(n->usb); }

static const struct dev_match hid_ids[] = {
    { DEVBUS_USB, DM_CLASS, { 0, 0, USB_CLASS_HID, 0, 0 }, 0 },
};
static const struct driver hid_driver = {
    "usb-hid", DG_USB_CLASS, 0, 1, hid_ids, hid_bind, hid_remove,
};
DRIVER_DECLARE(hid_driver);
