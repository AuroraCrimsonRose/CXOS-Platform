/* /CXK/kernel/drivers/usb/usb.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * USB core - everything that is true regardless of which host controller is
 * underneath.
 *
 * Enumeration, descriptor parsing and the device registry live here because
 * they are identical on EHCI, OHCI and xHCI: the wire protocol is the USB
 * spec's, not the controller's. Only the transport differs, and that is what
 * struct usb_hc_ops abstracts.
 *
 * ---- Where the controllers genuinely differ -------------------------------
 *
 * One asymmetry drove this design and is worth stating plainly: on EHCI and
 * OHCI the driver issues SET_ADDRESS itself as an ordinary control transfer,
 * but on xHCI it CANNOT - the controller owns the address space and assigns one
 * as part of its Address Device command. Worse, on xHCI no control transfer is
 * possible at all until a slot has been enabled and addressed, so the ordering
 * is inverted relative to the older controllers.
 *
 * So attach() is the seam. A host controller is handed a device on a port and
 * must return with it addressed and control() working; how it gets there is its
 * own business. Everything after that point is shared.
 */

#ifndef USB_H
#define USB_H

#include <stdint.h>

#define USB_MAX_DEVICES 16

/* Speeds as the USB spec orders them. LOW and FULL are here even though EHCI
   can never see them - OHCI and xHCI can, and the core has to describe them. */
enum usb_speed {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW,        /*   1.5 Mb/s */
    USB_SPEED_FULL,       /*    12 Mb/s */
    USB_SPEED_HIGH,       /*   480 Mb/s */
    USB_SPEED_SUPER,      /*     5 Gb/s */
};

const char *usb_speed_name(enum usb_speed s);

struct usb_device;

/* What a host controller must provide for the core to drive devices on it. */
struct usb_hc_ops {
    const char *name;

    /* Bring a freshly detected device to the point where it has an address and
       control() works, filling in dev->address and dev->max_packet0. Returns 1
       on success. */
    int (*attach)(struct usb_device *dev);

    /* Control transfer on endpoint 0. Returns bytes transferred, or -1. */
    int (*control)(struct usb_device *dev, uint8_t bm_request_type,
                   uint8_t b_request, uint16_t w_value, uint16_t w_index,
                   void *data, uint16_t w_length);

    /* Bulk transfer. `ep` is the endpoint ADDRESS from the descriptor, so bit 7
       already encodes direction. May be null until a controller implements it. */
    int (*bulk)(struct usb_device *dev, uint8_t ep, void *data, uint32_t len);
};

struct usb_device {
    const struct usb_hc_ops *ops;
    void    *hc;              /* controller-private handle                    */
    uint32_t slot;            /* controller-private id (xHCI slot), 0 if none */

    uint8_t  address;
    uint8_t  port;            /* root-hub port                                */
    uint8_t  max_packet0;     /* bMaxPacketSize0                              */
    enum usb_speed speed;

    uint8_t  dev_class;       /* bDeviceClass - usually 0, see if_class       */
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint16_t vendor_id;
    uint16_t product_id;

    /* From the configuration descriptor. A device reporting class 0 declares
       its real class on the interface instead - which every USB flash drive
       does, so these are the fields worth looking at. */
    uint8_t  configuration;   /* bConfigurationValue, already selected        */
    uint8_t  if_class;        /* 0x08 = mass storage, 0x03 = HID              */
    uint8_t  if_subclass;     /* 0x06 = SCSI transparent command set          */
    uint8_t  if_protocol;     /* 0x50 = bulk-only transport                   */
    uint8_t  ep_in;           /* bulk IN endpoint address, 0 if none          */
    uint8_t  ep_out;          /* bulk OUT endpoint address, 0 if none         */
    uint16_t ep_in_mps;
    uint16_t ep_out_mps;

    uint8_t  in_use;
};

/* ---- standard requests and descriptor types ----------------------------- */
#define USB_REQ_GET_DESCRIPTOR   0x06
#define USB_REQ_SET_ADDRESS      0x05
#define USB_REQ_SET_CONFIGURATION 0x09

#define USB_DESC_DEVICE          0x01
#define USB_DESC_CONFIG          0x02
#define USB_DESC_INTERFACE       0x04
#define USB_DESC_ENDPOINT        0x05

#define USB_CLASS_HID            0x03
#define USB_CLASS_MASS_STORAGE   0x08
#define USB_PROTO_BULK_ONLY      0x50

struct usb_device_descriptor {
    uint8_t  b_length;
    uint8_t  b_descriptor_type;
    uint16_t bcd_usb;
    uint8_t  b_device_class;
    uint8_t  b_device_subclass;
    uint8_t  b_device_protocol;
    uint8_t  b_max_packet_size0;
    uint16_t id_vendor;
    uint16_t id_product;
    uint16_t bcd_device;
    uint8_t  i_manufacturer;
    uint8_t  i_product;
    uint8_t  i_serial;
    uint8_t  b_num_configurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t  b_length;
    uint8_t  b_descriptor_type;
    uint16_t w_total_length;
    uint8_t  b_num_interfaces;
    uint8_t  b_configuration_value;
    uint8_t  i_configuration;
    uint8_t  bm_attributes;
    uint8_t  b_max_power;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t b_length;
    uint8_t b_descriptor_type;
    uint8_t b_interface_number;
    uint8_t b_alternate_setting;
    uint8_t b_num_endpoints;
    uint8_t b_interface_class;
    uint8_t b_interface_subclass;
    uint8_t b_interface_protocol;
    uint8_t i_interface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  b_length;
    uint8_t  b_descriptor_type;
    uint8_t  b_endpoint_address;
    uint8_t  bm_attributes;
    uint16_t w_max_packet_size;
    uint8_t  b_interval;
} __attribute__((packed));

/* The SETUP packet, 8 bytes, layout fixed by the spec. Controllers stage this
   into their own DMA memory; the core only fills it in. */
struct usb_setup {
    uint8_t  bm_request_type;
    uint8_t  b_request;
    uint16_t w_value;
    uint16_t w_index;
    uint16_t w_length;
} __attribute__((packed));

/* ---- core API ----------------------------------------------------------- */

/* Claim a device slot for a controller to fill in. Returns 0 when full. */
struct usb_device *usb_alloc_device(const struct usb_hc_ops *ops, void *hc,
                                    uint8_t port, enum usb_speed speed);
void usb_free_device(struct usb_device *dev);

/* Drive a newly attached device all the way to configured, and log it. Calls
   ops->attach() first. Returns 1 if the device is usable. */
int usb_enumerate(struct usb_device *dev);

/* GET_DESCRIPTOR helper, used by controllers during attach(). */
int usb_get_descriptor(struct usb_device *dev, uint8_t type, uint8_t index,
                       void *out, uint16_t len);

unsigned usb_device_count(void);
const struct usb_device *usb_get(unsigned index);

/* Bring up every USB host controller that is present. Call after pci_init(). */
void usb_init(void);

#endif
