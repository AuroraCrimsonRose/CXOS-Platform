// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_devtree.c */
/* Aurora Tejeda / CATX Systems */
/* The device tree's self-tests (Phase 2.1, drivers/devtree.h).
 *
 * The first four bind the tests' own nodes (DEVBUS_TEST) with their own
 * drivers (DG_TEST) through devtree_bind_with, so no real driver can match
 * and nothing the boot bound is touched: the rules. The fifth reads the tree
 * the boot built, so it is the test that the boot's binding applies them.
 * Each frees every node it made, on every path. */

#include <stdint.h>
#include "ktest_devtree.h"
#include "drivers/devtree.h"
#include "drivers/net/e1000.h"
#include "drivers/net/rtl8169.h"
#include "drivers/usb/xhci.h"
#include "drivers/usb/ehci.h"
#include "drivers/usb/ohci.h"
#include "logging.h"

static int dt_report(const char *why) {
    if (why) klog("KTEST", SEV_FAIL, why);
    return why == 0;
}

static int same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- the tests' drivers: each start is logged, and answers `answer` ---- */

#define LOG_MAX 8
static const struct driver *started[LOG_MAX];
static struct dev_node *started_on[LOG_MAX];
static unsigned nstarted, nremoved;

static int answer_of(const struct driver *d);

static void note(struct dev_node *n) {
    if (nstarted < LOG_MAX) { started[nstarted] = n->drv; started_on[nstarted] = n; }
    nstarted++;
}

static int t_start(struct dev_node *n)  { note(n); return answer_of(n->drv); }
static void t_remove(struct dev_node *n) { (void)n; nremoved++; }

/* The test nodes: vendor 0x1234, device 0x0042, class 0x0C/0x03/0x30. */
#define TV 0x1234u
#define TD 0x0042u

static const struct dev_match m_class[] = {   /* class, subclass, interface */
    { DEVBUS_TEST, DM_CLASS | DM_SUB | DM_PROG, { 0, 0, 0x0C, 0x03, 0x30 }, 0 },
};
static const struct dev_match m_vendor[] = {
    { DEVBUS_TEST, DM_VENDOR, { TV, 0, 0, 0, 0 }, 0 },
};
static const struct dev_match m_device[] = {  /* two fields, fewer than m_class's three */
    { DEVBUS_TEST, DM_VENDOR | DM_DEVICE, { TV, TD, 0, 0, 0 }, 0 },
};
static const struct dev_match m_any[] = {     /* anything on the bus */
    { DEVBUS_TEST, 0, { 0, 0, 0, 0, 0 }, 0 },
};
static const struct dev_match m_other[] = {   /* another device: never these nodes */
    { DEVBUS_TEST, DM_VENDOR | DM_DEVICE, { TV, TD + 1, 0, 0, 0 }, 0 },
};

static struct driver d_class  = { "t-class",  DG_TEST, 0, 1, m_class,  t_start, t_remove };
static struct driver d_vendor = { "t-vendor", DG_TEST, 0, 1, m_vendor, t_start, t_remove };
static struct driver d_device = { "t-device", DG_TEST, 0, 1, m_device, t_start, t_remove };
static struct driver d_any    = { "t-any",    DG_TEST, 0, 1, m_any,    t_start, t_remove };
static struct driver d_other  = { "t-other",  DG_TEST, 0, 1, m_other,  t_start, t_remove };

static int ans_class, ans_vendor, ans_device, ans_any, ans_other;

static int answer_of(const struct driver *d) {
    if (d == &d_class)  return ans_class;
    if (d == &d_vendor) return ans_vendor;
    if (d == &d_device) return ans_device;
    if (d == &d_any)    return ans_any;
    if (d == &d_other)  return ans_other;
    return 0;
}

static void reset(void) {
    nstarted = nremoved = 0;
    for (unsigned i = 0; i < LOG_MAX; i++) { started[i] = 0; started_on[i] = 0; }
    ans_class = ans_vendor = ans_device = ans_any = ans_other = 1;
    d_class.order = d_vendor.order = d_device.order = d_any.order = d_other.order = 0;
}

static struct dev_node *t_node(uint16_t vendor, uint16_t device) {
    struct dev_ids ids = { vendor, device, 0x0C, 0x03, 0x30 };
    return devtree_add(0, DEVBUS_TEST, &ids, 0);
}

static void t_free(struct dev_node **n, unsigned k) {
    for (unsigned i = 0; i < k; i++) if (n[i]) devtree_gone(n[i]);
}

/* ---- 1. the most specific driver wins ---- */

static const char *specific_steps(struct dev_node **n) {
    /* The weights: one heavier field outweighs all the lighter ones. */
    struct dev_node probe = { 0 };
    probe.bus = DEVBUS_TEST;
    probe.ids = (struct dev_ids){ TV, TD, 0x0C, 0x03, 0x30 };
    uint32_t s_class = devtree_score(&m_class[0], &probe);
    uint32_t s_vendor = devtree_score(&m_vendor[0], &probe);
    uint32_t s_device = devtree_score(&m_device[0], &probe);
    uint32_t s_any = devtree_score(&m_any[0], &probe);
    if (!s_any || !s_class)                return "devtree specific: a match scored 0";
    if (!(s_any < s_class))                return "devtree specific: any not below class";
    if (!(s_class < s_vendor))             return "devtree specific: class not below vendor";
    if (!(s_vendor < s_device))            return "devtree specific: vendor not below device";
    if (devtree_score(&m_other[0], &probe)) return "devtree specific: other device scored";
    probe.bus = DEVBUS_PCI;
    if (devtree_score(&m_any[0], &probe))   return "devtree specific: other bus scored";

    /* Bound: all five offered, listed least specific first so that list
       order cannot be what picks the winner. */
    const struct driver *const all[] = { &d_any, &d_class, &d_other, &d_vendor, &d_device };
    n[0] = t_node(TV, TD);        /* every driver but t-other */
    n[1] = t_node(TV, TD + 7);    /* t-vendor, t-class, t-any */
    n[2] = t_node(0x4321, TD);    /* t-class, t-any */
    if (!n[0] || !n[1] || !n[2]) return "devtree specific: setup nodes";
    unsigned b = devtree_bind_with(all, 5, DG_TEST, 0, -1);
    if (b != 3)                       return "devtree specific: not three bound";
    if (nstarted != 3)                return "devtree specific: a start too many";
    if (n[0]->drv != &d_device)       return "devtree specific: device lost";
    if (n[1]->drv != &d_vendor)       return "devtree specific: vendor lost";
    if (n[2]->drv != &d_class)        return "devtree specific: class lost";
    for (unsigned i = 0; i < 3; i++)
        if (n[i]->state != DEV_BOUND) return "devtree specific: not DEV_BOUND";
    if (devtree_bind_with(all, 5, DG_TEST, 0, -1) != 0) return "devtree specific: bound twice";
    return 0;
}

int ktest_devtree_specific(void) {
    reset();
    struct dev_node *n[3] = { 0 };
    const char *why = specific_steps(n);
    t_free(n, 3);
    return dt_report(why);
}

/* ---- 2. a declined start falls to the next-best ---- */

static const char *fallback_steps(struct dev_node **n) {
    const struct driver *const all[] = { &d_any, &d_vendor, &d_device };
    n[0] = t_node(TV, TD);
    if (!n[0]) return "devtree fallback: setup node";
    ans_device = 0;                          /* the best declines */
    if (devtree_bind_with(all, 3, DG_TEST, 0, -1) != 1) return "devtree fallback: not bound";
    if (nstarted != 2)                       return "devtree fallback: not two starts";
    if (started[0] != &d_device || started[1] != &d_vendor)
        return "devtree fallback: not best, then next";
    if (n[0]->drv != &d_vendor || n[0]->state != DEV_BOUND)
        return "devtree fallback: next-best not bound";

    /* Every driver declines: the node stays unbound, each tried once. */
    n[1] = t_node(TV, TD);
    if (!n[1]) return "devtree fallback: setup node";
    nstarted = 0;
    ans_device = ans_vendor = ans_any = 0;
    if (devtree_bind_with(all, 3, DG_TEST, 0, -1) != 0) return "devtree fallback: bound a decline";
    if (nstarted != 3)                       return "devtree fallback: not each once";
    if (started[2] != &d_any)                return "devtree fallback: any not last";
    if (n[1]->state != DEV_UNBOUND || n[1]->drv) return "devtree fallback: not left unbound";
    /* and is not offered again to the drivers that declined it */
    nstarted = 0;
    if (devtree_bind_with(all, 3, DG_TEST, 0, -1) != 0 || nstarted)
        return "devtree fallback: retried a decline";
    return 0;
}

int ktest_devtree_fallback(void) {
    reset();
    struct dev_node *n[2] = { 0 };
    const char *why = fallback_steps(n);
    t_free(n, 2);
    return dt_report(why);
}

/* ---- 3. start order within a group, and the group's limit ---- */

static const char *order_steps(struct dev_node **n) {
    /* n0 is t-class's, n1 is t-device's (t-class matches it too, lower
       score). t-device starts first because its order is lower, though its
       node is the later one. */
    const struct driver *const all[] = { &d_class, &d_device };
    n[0] = t_node(0x4321, TD);
    n[1] = t_node(TV, TD);
    if (!n[0] || !n[1]) return "devtree order: setup nodes";
    d_class.order = 2;
    d_device.order = 1;
    if (devtree_bind_with(all, 2, DG_TEST, 0, -1) != 2) return "devtree order: not two bound";
    if (started_on[0] != n[1] || started_on[1] != n[0]) return "devtree order: not by order";

    /* The same tree again, limited to one: only the first in order. */
    t_free(n, 2);
    n[0] = n[1] = 0;
    reset();
    d_class.order = 2;
    d_device.order = 1;
    n[0] = t_node(0x4321, TD);
    n[1] = t_node(TV, TD);
    if (!n[0] || !n[1]) return "devtree order: setup nodes";
    if (devtree_bind_with(all, 2, DG_TEST, 1, -1) != 1) return "devtree limit: not one bound";
    if (nstarted != 1 || n[1]->drv != &d_device)       return "devtree limit: not the first";
    if (n[0]->state != DEV_UNBOUND)                    return "devtree limit: past the limit";
    /* The limit counts what the group already holds. */
    if (devtree_bind_with(all, 2, DG_TEST, 1, -1) != 0) return "devtree limit: held not counted";
    if (devtree_bind_with(all, 2, DG_TEST, 2, -1) != 1 || n[0]->drv != &d_class)
        return "devtree limit: room not used";
    return 0;
}

int ktest_devtree_order(void) {
    reset();
    struct dev_node *n[2] = { 0 };
    const char *why = order_steps(n);
    t_free(n, 2);
    return dt_report(why);
}

/* ---- 4. a gone node runs its driver's remove and frees its slot ---- */

static const char *gone_steps(struct dev_node **n) {
    const struct driver *const all[] = { &d_device };
    unsigned live = 0;
    for (unsigned i = 0; i < DEVTREE_MAX; i++) live += devtree_get(i) != 0;
    n[0] = t_node(TV, TD);
    n[1] = t_node(0x4321, TD);              /* nobody's */
    if (!n[0] || !n[1]) return "devtree gone: setup nodes";
    uint16_t id0 = n[0]->id, id1 = n[1]->id;
    /* `only`: one node bound, the other left alone */
    if (devtree_bind_with(all, 1, DG_TEST, 0, id1) != 0) return "devtree gone: only bound another";
    if (devtree_bind_with(all, 1, DG_TEST, 0, id0) != 1) return "devtree gone: only not bound";
    devtree_gone(n[0]);
    devtree_gone(n[1]);
    n[0] = n[1] = 0;
    if (nremoved != 1)                     return "devtree gone: remove not run once";
    if (devtree_get(id0) || devtree_get(id1)) return "devtree gone: node not freed";
    unsigned now = 0;
    for (unsigned i = 0; i < DEVTREE_MAX; i++) now += devtree_get(i) != 0;
    if (now != live)                       return "devtree gone: count moved";
    return 0;
}

int ktest_devtree_gone(void) {
    reset();
    struct dev_node *n[2] = { 0 };
    const char *why = gone_steps(n);
    t_free(n, 2);
    return dt_report(why);
}

/* ---- 5. the boot's tree: bound by these rules ----
   Every bound device has a driver that matches it; every unbound one that a
   driver matches was declined by that driver, or is past its group's limit
   (DG_NET, one NIC). Each count a driver keeps agrees with the tree, and the
   section holds every driver this kernel has. */

#define DRIVERS_IN_KERNEL 9u   /* e1000 rtl8169 ata ahci xhci ehci ohci usb-hid usb-storage */

static unsigned bound_to(const char *name) {
    unsigned c = 0;
    for (unsigned i = 0; i < DEVTREE_MAX; i++) {
        const struct dev_node *n = devtree_get(i);
        if (n && n->state == DEV_BOUND && n->drv && same(n->drv->name, name)) c++;
    }
    return c;
}

static unsigned group_bound(uint8_t group) {
    unsigned c = 0;
    for (unsigned i = 0; i < DEVTREE_MAX; i++) {
        const struct dev_node *n = devtree_get(i);
        if (n && n->state == DEV_BOUND && n->drv && n->drv->group == group) c++;
    }
    return c;
}

static const char *boot_steps(void) {
    unsigned nd = devtree_drivers();
    if (nd != DRIVERS_IN_KERNEL) return "devtree boot: drivers not nine";
    int ata = 0;
    for (unsigned i = 0; i < DEVTREE_MAX; i++) {
        const struct dev_node *n = devtree_get(i);
        if (!n || n->bus == DEVBUS_ROOT || n->bus == DEVBUS_TEST) continue;
        if (n->state == DEV_BOUND) {
            if (!n->drv || !devtree_driver_score(n->drv, n)) return "devtree boot: bound, no match";
            if (n->bus == DEVBUS_PLATFORM && n->name && same(n->name, "pc-ata") &&
                same(n->drv->name, "ata")) ata = 1;
            continue;
        }
        if (n->state != DEV_UNBOUND) return "devtree boot: a node mid-bind";
        for (unsigned j = 0; j < nd; j++) {
            const struct driver *d = devtree_driver(j);
            if (!devtree_driver_score(d, n) || (n->tried & (1u << j))) continue;
            if (d->group == DG_NET && group_bound(DG_NET) >= 1) continue;
            return "devtree boot: unbound, untried match";
        }
    }
    if (!ata)                                    return "devtree boot: pc-ata not ata's";
    if (bound_to("e1000") != (unsigned)e1000_present())     return "devtree boot: e1000 disagrees";
    if (bound_to("rtl8169") != (unsigned)rtl8169_present()) return "devtree boot: rtl8169 disagrees";
    if (group_bound(DG_NET) > 1)                 return "devtree boot: two NICs";
    if (bound_to("xhci") != xhci_count())        return "devtree boot: xhci disagrees";
    if (bound_to("ehci") != ehci_count())        return "devtree boot: ehci disagrees";
    if (bound_to("ohci") != ohci_count())        return "devtree boot: ohci disagrees";
    if (devtree_bound() > devtree_count())       return "devtree boot: more bound than nodes";
    return 0;
}

int ktest_devtree_boot(void) { return dt_report(boot_steps()); }
