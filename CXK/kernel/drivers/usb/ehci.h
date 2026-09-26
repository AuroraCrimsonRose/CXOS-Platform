/* /CXK/kernel/drivers/usb/ehci.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * EHCI - USB 2.0 host controller. Stage 1: bring the controller up, enumerate
 * what is plugged in, and expose control transfers.
 *
 * ---- Why EHCI, and what it does NOT get you -------------------------------
 *
 * EHCI drives HIGH-SPEED (480 Mb/s) devices and nothing else. That is the whole
 * shape of this driver, and it is easy to get wrong:
 *
 *   - USB flash drives are high speed. EHCI handles them directly, which is why
 *     this is the route to USB storage.
 *   - Keyboards and mice are LOW or FULL speed. EHCI physically cannot talk to
 *     them. On real hardware they are handled by a companion controller -
 *     OHCI on AMD southbridges (SB950 included), UHCI on Intel.
 *
 * So this driver will never see a USB keyboard, and that is not a bug. What it
 * must do instead is hand those ports BACK to the companion controller, which
 * enumerate_port() does by setting the port-owner bit. Getting that wrong is
 * how an EHCI driver silently kills a machine's USB keyboard: CONFIGFLAG routes
 * every port to EHCI, and a full-speed device on an EHCI-owned port is reachable
 * by nobody.
 *
 * ---- The BIOS owns this controller before we do ---------------------------
 *
 * On real hardware the firmware is already driving EHCI - that is how a USB
 * keyboard works at the boot menu, via SMM traps that emulate a PS/2 controller.
 * Taking over requires the EECP handshake in ehci_bios_handoff(); without it
 * the BIOS keeps servicing the controller behind our back and the two fight.
 * QEMU does not need it, which is exactly why it has to be written from the
 * spec rather than discovered by testing.
 */

#ifndef EHCI_H
#define EHCI_H

#include <stdint.h>

#define USB_MAX_DEVICES 8

enum usb_speed { USB_SPEED_UNKNOWN = 0, USB_SPEED_HIGH };

struct usb_device {
    uint8_t  address;        /* assigned USB address (1..n)                */
    uint8_t  port;           /* root-hub port it is attached to            */
    uint8_t  max_packet0;    /* bMaxPacketSize0 - needed by every transfer */
    uint8_t  dev_class;      /* bDeviceClass (0 = look at the interface)   */
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    enum usb_speed speed;

    /* From the configuration descriptor. A device that reports class 0 declares
       its real class here instead - which every USB flash drive does, so the
       interface fields are the ones worth looking at, not dev_class. */
    uint8_t  configuration;      /* bConfigurationValue, already SET          */
    uint8_t  if_class;           /* 0x08 = mass storage                       */
    uint8_t  if_subclass;        /* 0x06 = SCSI transparent command set       */
    uint8_t  if_protocol;        /* 0x50 = bulk-only transport                */
    uint8_t  ep_in;              /* bulk IN endpoint address, 0 if none       */
    uint8_t  ep_out;             /* bulk OUT endpoint address, 0 if none      */
    uint16_t ep_in_mps;
    uint16_t ep_out_mps;
};

/* probe PCI for an EHCI controller and bring it up. returns 1 on success,
   0 if none present / init failed. */
int ehci_init(void);

int ehci_present(void);
unsigned ehci_device_count(void);
const struct usb_device *ehci_get(unsigned index);

/* Issue a control transfer on endpoint 0.
 *
 * This is the seam the mass-storage layer will build on: everything USB begins
 * with a control transfer, and bulk endpoints are the same queue-head machinery
 * with a different PID. Returns bytes transferred, or -1 on error.
 *
 * `data` may be null when wLength is 0. Transfers are limited to
 * EHCI_XFER_MAX bytes, which is all enumeration or a CBW/CSW ever needs. */
#define EHCI_XFER_MAX 4096

int ehci_control(uint8_t addr, uint8_t max_packet,
                 uint8_t bm_request_type, uint8_t b_request,
                 uint16_t w_value, uint16_t w_index,
                 void *data, uint16_t w_length);

#endif
