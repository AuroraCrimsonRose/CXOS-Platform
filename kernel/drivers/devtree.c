// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/devtree.c */
/* Aurora Tejeda / CATX Systems */
/* The device tree and scored driver matching (devtree.h). */

#include "devtree.h"
#include "bus/pci.h"
#include "usb/usb.h"
#include "logging.h"
#include "format.h"
#include "sync/spinlock.h"

static struct dev_node nodes[DEVTREE_MAX];
static spinlock_t devtree_lock = SPINLOCK_INIT(LR_DEVTREE);

/* .cxk_drivers, gathered by linker.ld: every DRIVER_DECLARE in the kernel. */
extern const struct driver *const __drivers_start[];
extern const struct driver *const __drivers_end[];

/* `tried` is a bit per driver. */
#define DRIVERS_MAX 32u

unsigned devtree_drivers(void) {
    unsigned n = (unsigned)(__drivers_end - __drivers_start);
    return n > DRIVERS_MAX ? DRIVERS_MAX : n;
}

const struct driver *devtree_driver(unsigned i) {
    return i < devtree_drivers() ? __drivers_start[i] : 0;
}

/* ---- matching -------------------------------------------------------------
 * A match on the right bus scores 1 for itself, then each field it names
 * adds its weight. Each weight is more than all the lighter ones together,
 * so one heavier field always wins: the device ID (32) outranks class,
 * subclass and interface together (14), and the vendor alone (16) still
 * outranks them, as a vendor's own driver should outrank a class driver. A
 * match naming nothing scores 1: a driver for anything on its bus, the
 * fallback every better one beats (the plain framebuffer, later). */
#define W_CLASS   2u
#define W_SUB     4u
#define W_PROG    8u
#define W_VENDOR 16u
#define W_DEVICE 32u
#define W_NAME   64u

static int str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

uint32_t devtree_score(const struct dev_match *m, const struct dev_node *n) {
    if (!m || !n || m->bus != n->bus) return 0;
    uint32_t s = 1;
    if (m->fields & DM_CLASS)  { if (m->ids.cls    != n->ids.cls)    return 0; s += W_CLASS;  }
    if (m->fields & DM_SUB)    { if (m->ids.sub    != n->ids.sub)    return 0; s += W_SUB;    }
    if (m->fields & DM_PROG)   { if (m->ids.prog   != n->ids.prog)   return 0; s += W_PROG;   }
    if (m->fields & DM_VENDOR) { if (m->ids.vendor != n->ids.vendor) return 0; s += W_VENDOR; }
    if (m->fields & DM_DEVICE) { if (m->ids.device != n->ids.device) return 0; s += W_DEVICE; }
    if (m->fields & DM_NAME)   { if (!str_eq(m->name, n->name))      return 0; s += W_NAME;   }
    return s;
}

uint32_t devtree_driver_score(const struct driver *d, const struct dev_node *n) {
    uint32_t best = 0;
    for (unsigned i = 0; d && i < d->nmatch; i++) {
        uint32_t s = devtree_score(&d->match[i], n);
        if (s > best) best = s;
    }
    return best;
}

/* ---- the table ------------------------------------------------------------ */

/* Under devtree_lock. */
static struct dev_node *node_new(uint16_t parent, uint8_t bus, const struct dev_ids *ids,
                                 const char *name) {
    for (unsigned i = 0; i < DEVTREE_MAX; i++) {
        struct dev_node *n = &nodes[i];
        if (n->state != DEV_FREE) continue;
        for (unsigned b = 0; b < sizeof *n; b++) ((uint8_t *)n)[b] = 0;
        n->id = (uint16_t)i;
        n->parent = parent < DEVTREE_MAX ? parent : 0;
        n->bus = bus;
        if (ids) n->ids = *ids;
        n->name = name;
        n->state = DEV_UNBOUND;
        return n;
    }
    return 0;
}

struct dev_node *devtree_add(uint16_t parent, uint8_t bus, const struct dev_ids *ids,
                             const char *name) {
    uint32_t f = spin_lock(&devtree_lock);
    struct dev_node *n = node_new(parent, bus, ids, name);
    spin_unlock(&devtree_lock, f);
    return n;
}

/* A bus is a node too, driven by the kernel itself: bound, with no driver,
   so nothing binds it and the report does not count it as a device. */
static struct dev_node *bus_node(uint16_t parent, const char *name) {
    struct dev_node *n = node_new(parent, DEVBUS_ROOT, 0, name);
    if (n) n->state = DEV_BOUND;
    return n;
}

void devtree_init(void) {
    uint32_t f = spin_lock(&devtree_lock);
    for (unsigned b = 0; b < sizeof nodes; b++) ((uint8_t *)nodes)[b] = 0;
    struct dev_node *root = bus_node(0, "system");
    struct dev_node *plat = bus_node(root->id, "platform");
    struct dev_node *pci  = bus_node(root->id, "pci");
    /* The legacy ATA channels, at their fixed ports: always there to be
       probed, as ata_init always probed them. */
    (void)node_new(plat->id, DEVBUS_PLATFORM, 0, "pc-ata");
    unsigned dropped = 0;
    for (unsigned i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_get(i);
        struct dev_ids ids = { d->vendor_id, d->device_id, d->class_code, d->subclass, d->prog_if };
        struct dev_node *n = node_new(pci->id, DEVBUS_PCI, &ids, 0);
        if (n) n->pci = d;
        else   dropped++;
    }
    spin_unlock(&devtree_lock, f);
    if (dropped)
        klog_u32("DEVTREE", SEV_WARN, "table full, PCI functions left out: ", dropped,
                 LOG_COLOR_VALUE, "");
}

/* ---- binding --------------------------------------------------------------
 * One start at a time per node, the tree lock released across it. Each turn
 * picks, among the unbound nodes, the one whose best untried driver has the
 * lowest `order` (the earlier node on a tie), claims it, and starts it. A
 * decline marks that driver tried on that node, so the next turn offers the
 * node its next-best. Every turn binds a node or marks a driver tried, so
 * the loop ends. */

static unsigned group_held(uint8_t group) {
    unsigned n = 0;
    for (unsigned i = 0; i < DEVTREE_MAX; i++)
        if ((nodes[i].state == DEV_BOUND || nodes[i].state == DEV_BINDING) &&
            nodes[i].drv && nodes[i].drv->group == group) n++;
    return n;
}

/* The best untried driver of the group for `n`: its index, or -1. A tie in
   score goes to the lower order, then to the earlier driver. */
static int best_driver(const struct driver *const *drv, unsigned ndrv, uint8_t group,
                       const struct dev_node *n) {
    int best = -1;
    uint32_t best_score = 0;
    for (unsigned j = 0; j < ndrv; j++) {
        const struct driver *d = drv[j];
        if (!d || d->group != group || (n->tried & (1u << j))) continue;
        uint32_t s = devtree_driver_score(d, n);
        if (!s) continue;
        if (best < 0 || s > best_score ||
            (s == best_score && d->order < drv[best]->order)) {
            best = (int)j;
            best_score = s;
        }
    }
    return best;
}

unsigned devtree_bind_with(const struct driver *const *drv, unsigned ndrv,
                           uint8_t group, unsigned limit, int only) {
    if (ndrv > DRIVERS_MAX) ndrv = DRIVERS_MAX;
    unsigned bound = 0;
    for (;;) {
        uint32_t f = spin_lock(&devtree_lock);
        if (limit && group_held(group) >= limit) { spin_unlock(&devtree_lock, f); break; }
        struct dev_node *pick = 0;
        int pick_drv = -1;
        unsigned lo = only >= 0 ? (unsigned)only : 0;
        unsigned hi = only >= 0 ? (unsigned)only + 1 : DEVTREE_MAX;
        for (unsigned i = lo; i < hi && i < DEVTREE_MAX; i++) {
            struct dev_node *n = &nodes[i];
            if (n->state != DEV_UNBOUND) continue;
            int j = best_driver(drv, ndrv, group, n);
            if (j < 0) continue;
            if (!pick || drv[j]->order < drv[pick_drv]->order) { pick = n; pick_drv = j; }
        }
        if (!pick) { spin_unlock(&devtree_lock, f); break; }
        const struct driver *d = drv[pick_drv];
        pick->state = DEV_BINDING;
        pick->drv = d;
        spin_unlock(&devtree_lock, f);

        int ok = d->start(pick);

        f = spin_lock(&devtree_lock);
        if (ok) { pick->state = DEV_BOUND; bound++; }
        else {
            pick->tried |= 1u << pick_drv;
            pick->drv = 0;
            pick->state = DEV_UNBOUND;
        }
        spin_unlock(&devtree_lock, f);
    }
    return bound;
}

unsigned devtree_bind_group(uint8_t group, unsigned limit) {
    return devtree_bind_with(__drivers_start, devtree_drivers(), group, limit, -1);
}

void devtree_gone(struct dev_node *n) {
    if (!n) return;
    uint32_t f = spin_lock(&devtree_lock);
    const struct driver *d = n->state == DEV_BOUND ? n->drv : 0;
    spin_unlock(&devtree_lock, f);
    if (d && d->remove) d->remove(n);
    f = spin_lock(&devtree_lock);
    n->state = DEV_FREE;
    n->drv = 0;
    spin_unlock(&devtree_lock, f);
}

int devtree_usb_attach(struct usb_device *dev) {
    struct dev_ids ids = { dev->vendor_id, dev->product_id,
                           dev->if_class, dev->if_subclass, dev->if_protocol };
    struct dev_node *n = devtree_add(dev->parent_node, DEVBUS_USB, &ids, 0);
    dev->node = n;
    if (!n) {
        klog("DEVTREE", SEV_WARN, "table full: a USB device is not driven");
        return 0;
    }
    n->usb = dev;
    return devtree_bind_with(__drivers_start, devtree_drivers(), DG_USB_CLASS, 0, n->id) != 0;
}

/* ---- reading -------------------------------------------------------------- */

const struct dev_node *devtree_get(unsigned id) {
    return id < DEVTREE_MAX && nodes[id].state != DEV_FREE ? &nodes[id] : 0;
}

const struct dev_node *devtree_find_pci(const struct pci_device *d) {
    for (unsigned i = 0; d && i < DEVTREE_MAX; i++)
        if (nodes[i].state != DEV_FREE && nodes[i].bus == DEVBUS_PCI && nodes[i].pci == d)
            return &nodes[i];
    return 0;
}

static int is_device(const struct dev_node *n) {
    return n->state != DEV_FREE && n->bus != DEVBUS_ROOT && n->bus != DEVBUS_TEST;
}

unsigned devtree_count(void) {
    unsigned c = 0;
    for (unsigned i = 0; i < DEVTREE_MAX; i++) c += is_device(&nodes[i]);
    return c;
}

unsigned devtree_bound(void) {
    unsigned c = 0;
    for (unsigned i = 0; i < DEVTREE_MAX; i++)
        c += is_device(&nodes[i]) && nodes[i].state == DEV_BOUND;
    return c;
}

/* "e1000  pci 00:03.0" or "usb-hid  usb 1234:5678": what is bound to what. */
static void describe(const struct dev_node *n, char *out, unsigned cap) {
    unsigned k = 0;
    char num[12];
    #define PUT(s) for (const char *p_ = (s); *p_ && k + 1 < cap; p_++) out[k++] = *p_
    #define HEX(v, w) do { num[fmt_hex(num, (uint32_t)(v), (w), 0)] = 0; PUT(num); } while (0)
    PUT(n->drv->name);
    if (n->bus == DEVBUS_PCI && n->pci) {
        PUT("  pci "); HEX(n->pci->bus, 2); PUT(":"); HEX(n->pci->slot, 2);
        PUT("."); HEX(n->pci->func, 1);
    } else if (n->bus == DEVBUS_USB) {
        PUT("  usb "); HEX(n->ids.vendor, 4); PUT(":"); HEX(n->ids.device, 4);
    } else {
        PUT("  "); PUT(n->name ? n->name : "?");
    }
    #undef HEX
    #undef PUT
    out[k] = 0;
}

void devtree_report(void) {
    klog_u32("DEVTREE", SEV_OK, "devices bound: ", devtree_bound(), LOG_COLOR_VALUE, "");
    klog_child_u32("of ", devtree_count(), LOG_COLOR_VALUE, "");
    char line[48];
    for (unsigned i = 0; i < DEVTREE_MAX; i++) {
        const struct dev_node *n = &nodes[i];
        if (!is_device(n) || n->state != DEV_BOUND || !n->drv) continue;
        describe(n, line, sizeof line);
        klog_child(line);
    }
}
