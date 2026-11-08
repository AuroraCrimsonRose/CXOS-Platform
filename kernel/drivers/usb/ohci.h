// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/usb/ohci.h */
/* Aurora Tejeda / CATX Systems */
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

/* The controllers that came up. Each is bound by the device tree
   (DG_USB_HOST, usb_init), which brings it up and enumerates what is
   attached. */
unsigned ohci_count(void);

/* Hot-plug: look at every root port once, enumerating a device that connected
   since the last look and dropping one that went away. A device EHCI hands
   over arrives late (it disconnects there and connects again here), so the
   one look ohci_init takes can miss it; this is what finds it. Sleeps: call
   from a thread holding usb_lock, never from an interrupt. */
void ohci_poll_ports(void);

/* One log line per controller: every root port's raw HcRhPortStatus, so a
   saved log (cxk rig bootlog) shows what each port reported. */
void ohci_report(void);

#endif
