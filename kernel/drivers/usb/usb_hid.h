/* /kernel/drivers/usb/usb_hid.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * USB HID - keyboards and mice, via the BOOT PROTOCOL.
 *
 * HID devices normally describe their reports with a report descriptor, which
 * is a small bytecode language and a real parser. The boot protocol exists to
 * avoid that: a device asked for it emits a fixed 8-byte keyboard report or
 * 3-byte mouse report instead, with no descriptor involved. Every BIOS on earth
 * relies on this, which is precisely why every keyboard implements it.
 *
 * ---- Why this driver is not optional ------------------------------------
 *
 * A USB keyboard works before any of this because the firmware emulates a PS/2
 * controller at port 0x60 from SMM. The moment ohci_init() takes the controller
 * away from SMM - which it must, to drive anything - that emulation stops. On a
 * machine whose keyboard is USB, a USB host driver WITHOUT a HID driver is
 * strictly worse than no USB driver at all: it takes the keyboard away and puts
 * nothing back.
 *
 * Decoded reports are pushed into the existing PS/2 keyboard and mouse paths,
 * so everything above - the shell's line editor, the GUI cursor - is unchanged
 * and cannot tell the difference.
 */

#ifndef USB_HID_H
#define USB_HID_H

struct usb_device;

/* Claim a device the core has enumerated, if it is a boot-protocol keyboard or
   mouse. Returns 1 if claimed. */
int usb_hid_attach(struct usb_device *dev);

/* Poll every claimed HID device and push what arrived into the input layer.
   Cheap and non-blocking when nothing is attached or nothing has happened. */
void usb_hid_poll(void);

int usb_hid_present(void);

#endif
