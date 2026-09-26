/* /CXK/kernel/drivers/usb/usb.c */
/* Aurora Tejeda / CATX Systems LLC */
/* USB core: enumeration, descriptor parsing, the device registry. See usb.h. */

#include "usb.h"
#include "ehci.h"
#include "ohci.h"
#include "xhci.h"
#include "usb_storage.h"
#include "logging.h"
#include "timer.h"

static struct usb_device devices[USB_MAX_DEVICES];
static unsigned ndevices = 0;

const char *usb_speed_name(enum usb_speed s) {
    switch (s) {
        case USB_SPEED_LOW:   return "low-speed";
        case USB_SPEED_FULL:  return "full-speed";
        case USB_SPEED_HIGH:  return "high-speed";
        case USB_SPEED_SUPER: return "SuperSpeed";
        default:              return "unknown-speed";
    }
}

struct usb_device *usb_alloc_device(const struct usb_hc_ops *ops, void *hc,
                                    uint8_t port, enum usb_speed speed) {
    for (unsigned i = 0; i < USB_MAX_DEVICES; i++) {
        if (devices[i].in_use) continue;
        struct usb_device *d = &devices[i];
        for (unsigned b = 0; b < sizeof(*d); b++) ((uint8_t *)d)[b] = 0;
        d->in_use = 1;
        d->ops = ops;
        d->hc = hc;
        d->port = port;
        d->speed = speed;
        d->max_packet0 = 0;      /* attach() fills this in */
        if (i + 1 > ndevices) ndevices = i + 1;
        return d;
    }
    return 0;
}

void usb_free_device(struct usb_device *dev) {
    if (!dev) return;
    dev->in_use = 0;
    /* ndevices is a high-water mark, not a live count - usb_get() checks
       in_use. Shrinking it would renumber devices behind anyone holding an
       index. */
}

int usb_get_descriptor(struct usb_device *dev, uint8_t type, uint8_t index,
                       void *out, uint16_t len) {
    if (!dev || !dev->ops || !dev->ops->control) return -1;
    return dev->ops->control(dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)((uint16_t)type << 8 | index), 0, out, len);
}

/* Read the configuration, record the first interface and its bulk endpoints,
   then select the configuration. A device is not usable until
   SET_CONFIGURATION has been issued - before that it answers control transfers
   and nothing else. */
static void read_configuration(struct usb_device *dev) {
    static uint8_t cfg[512];

    struct usb_config_descriptor head;
    for (unsigned i = 0; i < sizeof(head); i++) ((uint8_t *)&head)[i] = 0;
    if (usb_get_descriptor(dev, USB_DESC_CONFIG, 0, &head, sizeof(head))
            < (int)sizeof(head))
        return;

    uint16_t total = head.w_total_length;
    if (total > sizeof(cfg)) total = sizeof(cfg);
    if (usb_get_descriptor(dev, USB_DESC_CONFIG, 0, cfg, total) < (int)total)
        return;

    dev->configuration = head.b_configuration_value;

    /* The descriptors arrive as one blob and are walked by stepping bLength at
       a time. They are interleaved and NOT in a fixed order, so indexing into
       them is wrong even when it happens to work on one device. */
    unsigned off = 0;
    int in_first_interface = 0;
    while (off + 2 <= total) {
        uint8_t len = cfg[off];
        uint8_t type = cfg[off + 1];
        if (len == 0 || off + len > total) break;   /* malformed: stop, do not spin */

        if (type == USB_DESC_INTERFACE) {
            const struct usb_interface_descriptor *id =
                (const struct usb_interface_descriptor *)&cfg[off];
            if (!dev->if_class && !in_first_interface) {
                dev->if_class = id->b_interface_class;
                dev->if_subclass = id->b_interface_subclass;
                dev->if_protocol = id->b_interface_protocol;
                in_first_interface = 1;
            } else {
                in_first_interface = 0;     /* only the first interface's endpoints */
            }
        } else if (type == USB_DESC_ENDPOINT && in_first_interface) {
            const struct usb_endpoint_descriptor *ed =
                (const struct usb_endpoint_descriptor *)&cfg[off];
            if ((ed->bm_attributes & 0x03) == 0x02) {       /* bulk */
                if (ed->b_endpoint_address & 0x80) {
                    dev->ep_in = ed->b_endpoint_address;
                    dev->ep_in_mps = ed->w_max_packet_size;
                } else {
                    dev->ep_out = ed->b_endpoint_address;
                    dev->ep_out_mps = ed->w_max_packet_size;
                }
            }
        }
        off += len;
    }

    if (dev->configuration)
        dev->ops->control(dev, 0x00, USB_REQ_SET_CONFIGURATION,
                          dev->configuration, 0, 0, 0);
}

int usb_enumerate(struct usb_device *dev) {
    if (!dev || !dev->ops || !dev->ops->attach) return 0;

    /* The controller gets the device addressed and control() working. What that
       takes is wildly different per controller - see the note in usb.h. */
    if (!dev->ops->attach(dev)) {
        klog_u32(dev->ops->name, SEV_WARN, "port ", dev->port, LOG_COLOR_VALUE,
                 ": device could not be addressed");
        usb_free_device(dev);
        return 0;
    }

    struct usb_device_descriptor dd;
    for (unsigned i = 0; i < sizeof(dd); i++) ((uint8_t *)&dd)[i] = 0;
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &dd, sizeof(dd)) < (int)sizeof(dd)) {
        klog_u32(dev->ops->name, SEV_WARN, "port ", dev->port, LOG_COLOR_VALUE,
                 ": device descriptor read failed");
        usb_free_device(dev);
        return 0;
    }

    dev->dev_class = dd.b_device_class;
    dev->dev_subclass = dd.b_device_subclass;
    dev->dev_protocol = dd.b_device_protocol;
    dev->vendor_id = dd.id_vendor;
    dev->product_id = dd.id_product;

    read_configuration(dev);

    klog_u32(dev->ops->name, SEV_OK, "port ", dev->port, LOG_COLOR_VALUE, "");
    klog_child(usb_speed_name(dev->speed));
    klog_child_u32("  vendor  ", dev->vendor_id, LOG_COLOR_VALUE, "");
    klog_child_u32("  product ", dev->product_id, LOG_COLOR_VALUE, "");
    /* Report the INTERFACE class: mass storage sets bDeviceClass to 0 and
       declares 0x08 here, so reporting the device class would call every flash
       drive "class 0". */
    klog_child_u32("  class   ", dev->if_class, LOG_COLOR_VALUE,
                   (dev->if_class == USB_CLASS_MASS_STORAGE &&
                    dev->if_protocol == USB_PROTO_BULK_ONLY)
                       ? " (mass storage, bulk-only)"
                       : (dev->if_class == USB_CLASS_HID ? " (HID)" : ""));
    if (dev->ep_in || dev->ep_out) {
        klog_child_u32("  bulk in ", dev->ep_in, LOG_COLOR_VALUE, "");
        klog_child_u32("  bulk out", dev->ep_out, LOG_COLOR_VALUE, "");
    }

    /* Offer it to the class drivers. Mass storage is the only one so far; HID
       would hook in here the same way. A device nobody claims is still
       enumerated and listed, just not driven. */
    usb_storage_attach(dev);
    return 1;
}

unsigned usb_device_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < ndevices; i++) if (devices[i].in_use) n++;
    return n;
}

const struct usb_device *usb_get(unsigned index) {
    unsigned n = 0;
    for (unsigned i = 0; i < ndevices; i++) {
        if (!devices[i].in_use) continue;
        if (n++ == index) return &devices[i];
    }
    return 0;
}

void usb_init(void) {
    /* xHCI first, deliberately. On hardware carrying both, xHCI handles every
       speed by itself while EHCI needs companions - so where a port could be
       driven by either, the better controller should claim it. On this era of
       AMD hardware they are separate silicon and both will come up. */
    int any = 0;
    any |= xhci_init();
    any |= ehci_init();
    any |= ohci_init();

    if (!any) return;      /* no controllers: say nothing, this is the norm */

    unsigned n = usb_device_count();
    if (n == 0) klog("USB", SEV_INFO, "no devices attached");
    else        klog_u32("USB", SEV_OK, "devices: ", n, LOG_COLOR_VALUE, "");
}
