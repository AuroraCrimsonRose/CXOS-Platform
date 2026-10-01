/* /kernel/drivers/usb/ohci.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * OHCI - USB 1.1 host controller, low and full speed.
 *
 * This is the one that matters on the AM3+ target. An SB950 exposes EHCI for
 * high-speed devices and OHCI for everything else, and "everything else" is
 * keyboards and mice - which are low or full speed and which EHCI physically
 * cannot talk to. Without this driver, the EHCI driver hands those ports to a
 * companion that has nobody home.
 *
 * Unlike EHCI's queue heads or xHCI's rings, OHCI is built from Endpoint
 * Descriptors holding a linked list of Transfer Descriptors, with the
 * controller reading a shared Host Controller Communications Area for its head
 * pointers. Physically different, same three-stage control transfer.
 */

#ifndef OHCI_H
#define OHCI_H

/* probe PCI for an OHCI controller and bring it up, enumerating what is
   attached. returns 1 if a controller came up, 0 if none present. */
int ohci_init(void);

#endif
