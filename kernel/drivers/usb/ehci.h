/* /kernel/drivers/usb/ehci.h */
/* Aurora Tejeda / CATX Systems */
/*
 * EHCI - USB 2.0 host controller.
 *
 * EHCI drives HIGH-SPEED (480 Mb/s) devices and nothing else. Keyboards and
 * mice are low or full speed and belong to a companion controller - OHCI on
 * AMD southbridges, UHCI on Intel. But CONFIGFLAG routes every root port to
 * EHCI, so this driver MUST hand back the ports it cannot drive or those
 * devices become unreachable by anybody. That is how an EHCI driver silently
 * kills a machine's USB keyboard.
 *
 * Everything above the transport - enumeration, descriptors, the device model
 * - lives in usb.c and is shared with OHCI and xHCI.
 */

#ifndef EHCI_H
#define EHCI_H

/* probe PCI for an EHCI controller and bring it up, enumerating what is
   attached. returns 1 if a controller came up, 0 if none present. */
int ehci_init(void);

#endif
