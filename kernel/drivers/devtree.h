// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/devtree.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The device tree and scored driver matching (Phase 2.1, kernel/CX_DRIVERS.md
 * §3.1, §4 step 1).
 *
 * One tree of what the machine has: a root, a platform bus under it (the
 * legacy ATA channels), every PCI function, and every USB device under the
 * PCI function of its host controller. A node carries its identity (vendor,
 * device, class, subclass, interface) and, once bound, the driver driving it.
 *
 * A driver declares what it supports in a table of matches in its own file
 * and registers itself with DRIVER_DECLARE, which puts it in the .cxk_drivers
 * section (linker.ld). Nothing lists the drivers by hand. The system binds:
 *
 *   - Score. A match scores the sum of the fields it names, weighted so that
 *     naming the device beats naming its class: a vendor-and-device driver
 *     outranks any class driver (XNU's probe scores, Haiku's
 *     supports_device; NT's hardware ID before its compatible ID). A field
 *     named and not equal is no match at all.
 *   - Fallback. A driver's start may decline (0): the node's next-best
 *     driver is tried, and only when none is left does it stay unbound.
 *   - Groups. Drivers bind in groups at the points the boot asks for them
 *     (devtree_bind_group), so the storage drivers still claim the low disk
 *     indices before USB can (kmain.c). Within a group, `order` decides who
 *     starts first: xHCI before EHCI before its OHCI companions, e1000
 *     before the RTL8111. A group's limit is the class's rule, not the
 *     drivers': netif drives one NIC.
 *
 * Today every driver is still C in the kernel and starts at boot; this is
 * the registry the user-space drivers of the later steps bind through.
 * Locking: the table is under devtree_lock (LR_DEVTREE, nothing under it but
 * the log). A start runs with no tree lock held: the node is claimed first
 * (DEV_BINDING), so two binders never start one node.
 */
#ifndef CXK_DEVTREE_H
#define CXK_DEVTREE_H

#include <stdint.h>

struct pci_device;
struct usb_device;

#define DEVTREE_MAX 320   /* PCI_MAX_DEVICES, the USB devices, the root and platform */

enum dev_bus {
    DEVBUS_ROOT = 0,
    DEVBUS_PLATFORM,      /* fixed, legacy hardware: named, not identified */
    DEVBUS_PCI,
    DEVBUS_USB,           /* ids: vendor, product; class = the interface's */
    DEVBUS_TEST,          /* the self-tests' own nodes: no real driver matches */
};

enum dev_state {
    DEV_FREE = 0,
    DEV_UNBOUND,          /* no driver has it (none matched, or all declined) */
    DEV_BINDING,          /* claimed by a binder, its start running */
    DEV_BOUND,
};

/* The groups, in the order the boot binds them (kmain.c). */
enum dev_group {
    DG_NET = 1,
    DG_STORAGE,
    DG_USB_HOST,
    DG_USB_CLASS,         /* bound per device as it is enumerated */
    DG_TEST,
    DG_COUNT
};

struct dev_ids {
    uint16_t vendor, device;
    uint8_t  cls, sub, prog;
};

struct driver;

struct dev_node {
    uint16_t id;          /* its index; stable while the node lives */
    uint16_t parent;      /* the root is its own parent */
    uint8_t  bus;         /* enum dev_bus */
    uint8_t  state;       /* enum dev_state */
    struct dev_ids ids;
    const char *name;     /* a platform node's, matched by DM_NAME */
    const struct pci_device *pci;   /* DEVBUS_PCI */
    struct usb_device *usb;         /* DEVBUS_USB */
    const struct driver *drv;       /* DEV_BOUND, DEV_BINDING */
    uint32_t tried;       /* drivers (by index) that declined it */
};

/* The fields a match names. */
#define DM_VENDOR 0x01u
#define DM_DEVICE 0x02u
#define DM_CLASS  0x04u
#define DM_SUB    0x08u
#define DM_PROG   0x10u
#define DM_NAME   0x20u

struct dev_match {
    uint8_t  bus;         /* enum dev_bus */
    uint8_t  fields;      /* DM_* */
    struct dev_ids ids;
    const char *name;
};

struct driver {
    const char *name;
    uint8_t  group;       /* enum dev_group */
    uint8_t  order;       /* within the group, lower starts first */
    uint8_t  nmatch;
    const struct dev_match *match;
    /* 1: driving it. 0: declined, and the next-best driver is tried. */
    int  (*start)(struct dev_node *n);
    /* The device is gone (devtree_gone). Optional. */
    void (*remove)(struct dev_node *n);
};

#define DRIVER_DECLARE(d) \
    static const struct driver *const drv_entry_##d \
        __attribute__((used, section(".cxk_drivers"))) = &d

/* The score of `m` against `n`: 0 when it does not match. The weights make
   DM_NAME > DM_DEVICE > DM_VENDOR > DM_PROG > DM_SUB > DM_CLASS, and the
   device's weight beats every class field together (devtree.c). */
uint32_t devtree_score(const struct dev_match *m, const struct dev_node *n);
/* The best of a driver's matches against `n`. */
uint32_t devtree_driver_score(const struct driver *d, const struct dev_node *n);

/* The root and platform nodes, then one node per PCI function. After
   pci_init. */
void devtree_init(void);

/* Bind the unbound nodes the group's drivers match, best driver first, the
   next-best on a decline; start order by `order`, then by node. `limit`
   caps the nodes bound in the group (0: none). Returns how many it bound. */
unsigned devtree_bind_group(uint8_t group, unsigned limit);

/* The same with an explicit driver list (the self-tests), and optionally
   one node only (`only`, or -1). */
unsigned devtree_bind_with(const struct driver *const *drv, unsigned ndrv,
                           uint8_t group, unsigned limit, int only);

/* A new node under `parent`, unbound, or 0 if the table is full. */
struct dev_node *devtree_add(uint16_t parent, uint8_t bus, const struct dev_ids *ids,
                             const char *name);
/* The device is gone: its driver's remove, then the node is freed. Children
   are not walked: today's USB devices have none. */
void devtree_gone(struct dev_node *n);

/* A USB device just enumerated: its node, under its controller's
   (dev->parent_node), then the USB class drivers bound to it. 1 if a driver
   took it. A full table leaves the device undriven, and says so. */
int devtree_usb_attach(struct usb_device *dev);

const struct dev_node *devtree_get(unsigned id);   /* 0 if free */
const struct dev_node *devtree_find_pci(const struct pci_device *d);
unsigned devtree_count(void);                      /* nodes in use */
unsigned devtree_bound(void);                      /* of them, bound */
unsigned devtree_drivers(void);                    /* in .cxk_drivers */
const struct driver *devtree_driver(unsigned i);

/* One line per bound node, and the totals. */
void devtree_report(void);

#endif
