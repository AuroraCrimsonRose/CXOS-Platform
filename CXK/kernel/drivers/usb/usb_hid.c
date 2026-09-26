/* /CXK/kernel/drivers/usb/usb_hid.c */
/* Aurora Tejeda / CATX Systems LLC */
/* USB HID boot-protocol keyboard and mouse. See usb_hid.h. */

#include "usb_hid.h"
#include "usb.h"
#include "keyboard.h"
#include "mouse.h"
#include "logging.h"

/* HID class requests, on the interface (bmRequestType 0x21) */
#define HID_REQ_SET_PROTOCOL  0x0B
#define HID_REQ_SET_IDLE      0x0A
#define HID_PROTOCOL_BOOT     0

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
};

static struct hid_device hids[MAX_HID];
static int any_present = 0;

/* ---- HID usage id -> the codes keyboard.c already produces ---------------
 * Usage ids are NOT ASCII and NOT PS/2 scancodes; they are their own numbering,
 * dense from 0x04 for 'a'. Two tables, unshifted and shifted, indexed directly
 * by usage id. Zero means "nothing printable", which the caller skips. */
static const char usage_lower[0x68] = {
    /* 0x00 */ 0, 0, 0, 0,
    /* 0x04 */ 'a','b','c','d','e','f','g','h','i','j','k','l','m',
               'n','o','p','q','r','s','t','u','v','w','x','y','z',
    /* 0x1E */ '1','2','3','4','5','6','7','8','9','0',
    /* 0x28 */ '\n', 0x1B, '\b', '\t', ' ',
    /* 0x2D */ '-','=','[',']','\\',
    /* 0x32 */ 0,           /* non-US # */
    /* 0x33 */ ';','\'','`',',','.','/',
    /* 0x39 */ 0,           /* caps lock */
    /* 0x3A */ 0,0,0,0,0,0,0,0,0,0,0,0,   /* F1-F12 */
    /* 0x46 */ 0,0,0,       /* print screen, scroll lock, pause */
    /* 0x49 */ 0,           /* insert */
    /* 0x4A */ 0,0,0,0,0,0,0,0,  /* home..up, handled as specials below */
    /* 0x52 */ 0,
    /* 0x53 */ 0,           /* num lock */
    /* 0x54 */ '/','*','-','+','\n',
    /* 0x59 */ '1','2','3','4','5','6','7','8','9','0','.',
    /* 0x64 */ 0,0,0,0,
};

static const char usage_upper[0x68] = {
    /* 0x00 */ 0, 0, 0, 0,
    /* 0x04 */ 'A','B','C','D','E','F','G','H','I','J','K','L','M',
               'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    /* 0x1E */ '!','@','#','$','%','^','&','*','(',')',
    /* 0x28 */ '\n', 0x1B, '\b', '\t', ' ',
    /* 0x2D */ '_','+','{','}','|',
    /* 0x32 */ 0,
    /* 0x33 */ ':','"','~','<','>','?',
    /* 0x39 */ 0,
    /* 0x3A */ 0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x46 */ 0,0,0,
    /* 0x49 */ 0,
    /* 0x4A */ 0,0,0,0,0,0,0,0,
    /* 0x52 */ 0,
    /* 0x53 */ 0,
    /* 0x54 */ '/','*','-','+','\n',
    /* 0x59 */ '1','2','3','4','5','6','7','8','9','0','.',
    /* 0x64 */ 0,0,0,0,
};

/* Arrow and navigation keys have no ASCII, and keyboard.h already defines codes
   for them that the shell's line editor understands. */
static unsigned char usage_special(uint8_t usage, int shift) {
    switch (usage) {
        case 0x4A: return KEY_HOME;
        case 0x4B: return KEY_PGUP;
        case 0x4D: return KEY_END;
        case 0x4E: return KEY_PGDN;
        case 0x4F: return KEY_RIGHT;
        case 0x50: return KEY_LEFT;
        case 0x51: return shift ? KEY_SHIFT_DN : KEY_DOWN;
        case 0x52: return shift ? KEY_SHIFT_UP : KEY_UP;
        default:   return 0;
    }
}

/* ---- the reports --------------------------------------------------------
 * Keyboard, 8 bytes: modifiers, reserved, then up to six usage ids of the keys
 * CURRENTLY HELD. It is a state snapshot, not an event - a key held down
 * reappears in every report. So a keystroke is the difference between this
 * report and the last one, which is what prev_keys is for. Treating each report
 * as an event would repeat every held key at the poll rate. */
static void poll_keyboard(struct hid_device *h) {
    uint8_t rpt[8];
    for (int i = 0; i < 8; i++) rpt[i] = 0;

    struct usb_device *dev = h->dev;
    int n = dev->ops->interrupt_poll(dev, dev->ep_int_in, rpt, 8);
    if (n < 8) return;               /* nothing new, or a runt report */

    uint8_t mods = rpt[0];
    int shift = (mods & 0x02) || (mods & 0x20);
    int ctrl  = (mods & 0x01) || (mods & 0x10);

    for (int i = 2; i < 8; i++) {
        uint8_t u = rpt[i];
        if (u == 0 || u == 1) continue;      /* 1 = rollover error */

        /* only report keys that were NOT held in the previous report */
        int was_held = 0;
        for (int j = 0; j < 6; j++) if (h->prev_keys[j] == u) { was_held = 1; break; }
        if (was_held) continue;

        unsigned char c = usage_special(u, shift);
        if (!c && u < 0x68) c = (unsigned char)(shift ? usage_upper[u] : usage_lower[u]);
        if (!c) continue;

        /* Ctrl+letter collapses to a control code, the same convention the PS/2
           path uses - Ctrl+C must reach the shell as 0x03. */
        if (ctrl && c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 1);
        else if (ctrl && c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 1);

        keyboard_inject(c);
    }

    for (int i = 0; i < 6; i++) h->prev_keys[i] = rpt[i + 2];
    h->prev_mods = mods;
}

/* Mouse, 3 bytes: buttons, then SIGNED X and Y deltas. Y is positive DOWNWARD,
   which already matches screen coordinates - no inversion. */
static void poll_mouse(struct hid_device *h) {
    uint8_t rpt[4];
    for (int i = 0; i < 4; i++) rpt[i] = 0;

    struct usb_device *dev = h->dev;
    int n = dev->ops->interrupt_poll(dev, dev->ep_int_in, rpt, 4);
    if (n < 3) return;

    int8_t dx = (int8_t)rpt[1];
    int8_t dy = (int8_t)rpt[2];
    mouse_inject(dx, dy, (uint32_t)(rpt[0] & 0x07));
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

    /* SET_PROTOCOL(boot). A device powers up in report mode, and without this
       it sends reports in whatever shape its descriptor declares - which we
       cannot read. wValue is the protocol, wIndex the interface. */
    dev->ops->control(dev, 0x21, HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT, 0, 0, 0);

    /* SET_IDLE(0) - report only on change rather than re-sending forever. Some
       devices STALL this; it is an optimisation, not a requirement. */
    dev->ops->control(dev, 0x21, HID_REQ_SET_IDLE, 0, 0, 0, 0);

    any_present = 1;
    klog("USBHID", SEV_OK, kbd ? "keyboard online (boot protocol)"
                               : "mouse online (boot protocol)");
    return 1;
}

void usb_hid_poll(void) {
    if (!any_present) return;
    for (int i = 0; i < MAX_HID; i++) {
        if (!hids[i].in_use) continue;
        if (hids[i].is_keyboard) poll_keyboard(&hids[i]);
        else                     poll_mouse(&hids[i]);
    }
}

int usb_hid_present(void) { return any_present; }
