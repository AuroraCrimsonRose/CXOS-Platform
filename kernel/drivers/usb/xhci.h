/* /kernel/drivers/usb/xhci.h */
/* Aurora Tejeda / CATX Systems */
/*
 * xHCI - USB 3.x host controller, and the one that matters most going forward.
 *
 * Unlike EHCI it is not "the USB 3 controller": xHCI drives EVERY speed - low,
 * full, high and SuperSpeed - from one controller with no companions and no
 * port handoff. On any chipset from roughly 2015 on it is the only USB
 * controller present, so one driver covers all modern hardware.
 *
 * It is also architecturally unlike the older controllers in a way that shapes
 * the whole USB core: the driver does not address devices. It asks the
 * controller to, via the Address Device command, and until a slot is enabled
 * and addressed no control transfer is possible at all. That inversion is why
 * usb_hc_ops has an attach() hook rather than the core issuing SET_ADDRESS.
 */

#ifndef XHCI_H
#define XHCI_H

/* probe PCI for an xHCI controller and bring it up, enumerating what is
   attached. returns 1 if a controller came up, 0 if none present. */
int xhci_init(void);

#endif
