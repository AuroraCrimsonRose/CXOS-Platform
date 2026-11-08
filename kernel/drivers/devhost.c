// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/devhost.c */
/* Aurora Tejeda / CATX Systems */
/* The driver host's primitives: devices, their registers, interrupts and DMA
   buffers, for drivers in processes (devhost.h). */

#include "drivers/devhost.h"
#include "drivers/devtree.h"
#include "object/kmem.h"
#include "object/kwait.h"
#include "object/htab.h"
#include "memman/ocache.h"
#include "memman/heap.h"
#include "arch/irq.h"
#include "arch/paging.h"
#include "sched.h"
#include "proc.h"
#include "usermode.h"
#include "systable.h"
#include "logging.h"
#include "cxk_abi.h"
#include "panic.h"

_Static_assert(KOBJ_DEVICE == OBJ_TYPE_DEVICE, "object_basic.kind is the kernel type");
_Static_assert(KOBJ_IRQ == OBJ_TYPE_IRQ, "object_basic.kind is the kernel type");
_Static_assert(DEVTREE_MAX == DEVICE_MAX, "a device id is a node's index");
_Static_assert(DEVICE_STATE_UNBOUND == DEV_UNBOUND && DEVICE_STATE_BINDING == DEV_BINDING &&
               DEVICE_STATE_BOUND == DEV_BOUND && DEVICE_STATE_HELD == DEV_HELD,
               "device_desc.state is the node's");
_Static_assert(DEVICE_BUS_ROOT == DEVBUS_ROOT && DEVICE_BUS_PLATFORM == DEVBUS_PLATFORM &&
               DEVICE_BUS_PCI == DEVBUS_PCI && DEVICE_BUS_USB == DEVBUS_USB,
               "device_desc.bus is the node's");

#define PCI_CMD_MEMORY     0x2u
#define PCI_CMD_BUS_MASTER 0x4u

static struct ocache device_cache;
static struct ocache irq_cache;

/* ---- devices -------------------------------------------------------------- */

void devhost_get(struct kdevice *d) { kobj_ref(&d->hdr); }
void devhost_put(struct kdevice *d) { kobj_unref(&d->hdr); }

void devhost_dma_retire(struct kdevice *d, struct kmem *m) {
    struct kmem *head = __atomic_load_n(&d->retired, __ATOMIC_RELAXED);
    do {
        m->qnext = head;
    } while (!__atomic_compare_exchange_n(&d->retired, &head, m, 1,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    devhost_put(d);
}

uint32_t devhost_retired(const struct kdevice *d) {
    uint32_t n = 0;
    for (const struct kmem *m = __atomic_load_n(&d->retired, __ATOMIC_ACQUIRE); m; m = m->qnext) n++;
    return n;
}

/* Nothing of the driver's refers to the device any more: no handle, no BAR
   object, no interrupt, no buffer. Quiet it, and only then give back the
   pages it may have been writing. */
static void device_destroy(struct kobj *o) {
    struct kdevice *d = (struct kdevice *)o;
    const struct pci_device *p = d->pci;
    pci_msi_disable(p);
    uint32_t cmd = pci_config_read32(p->bus, p->slot, p->func, 0x04);
    cmd &= ~PCI_CMD_BUS_MASTER;
    if (!(d->command & PCI_CMD_MEMORY)) cmd &= ~PCI_CMD_MEMORY;
    pci_config_write32(p->bus, p->slot, p->func, 0x04, cmd);
    /* The write above reaches the device before this read completes: a
       config read is not posted, so DMA has stopped once it returns. */
    (void)pci_config_read32(p->bus, p->slot, p->func, 0x04);

    struct kmem *m = __atomic_exchange_n(&d->retired, 0, __ATOMIC_ACQUIRE);
    while (m) {
        struct kmem *next = m->qnext;
        kmem_dma_release(m);
        m = next;
    }
    devtree_release(d->node);
    ocache_free(&device_cache, d);
}

static const struct kobj_ops device_ops = {
    .name         = "device",
    .rights_valid = HRIGHT_BAR | HRIGHT_IRQ | HRIGHT_DMA,
    /* a supervisor opens it and hands it to the driver it starts */
    .transfer     = 1,
    .destroy      = device_destroy,
};

/* ---- interrupts ------------------------------------------------------------- */

struct kirq {
    struct kobj hdr;          /* first; KOBJ_IRQ */
    struct kdevice *dev;      /* a reference */
    int vector;
    uint32_t count;           /* since the last ack; under the wait lock */
};

/* Which interrupt object each vector raises; under the wait lock. */
static struct kirq *by_vector[256];

/* An MSI from a held device: count it and signal, under the lock the waiters
   and the ack take. Interrupts are off. */
static void devhost_isr(struct registers *r) {
    uint32_t v = irq_frame_vector(r) & 0xFFu;
    uint32_t f = kwait_lock();
    struct kirq *k = by_vector[v];
    if (k) {
        k->count++;
        kobj_signal_locked(&k->hdr, SIGNAL_IRQ_FIRED);
    }
    kwait_unlock(f);
}

static void irq_destroy(struct kobj *o) {
    struct kirq *k = (struct kirq *)o;
    struct kdevice *d = k->dev;
    /* The device stops sending first; an interrupt already on its way finds
       no object once the entry is gone, and the vector is free after. */
    pci_msi_disable(d->pci);
    uint32_t f = kwait_lock();
    by_vector[k->vector & 0xFF] = 0;
    kwait_unlock(f);
    irq_free_msi_vector(k->vector);
    __atomic_store_n(&d->irq_taken, 0, __ATOMIC_RELEASE);
    ocache_free(&irq_cache, k);
    devhost_put(d);
}

static const struct kobj_ops irq_ops = {
    .name          = "interrupt",
    .rights_valid  = HRIGHT_ACK,
    .signals_valid = SIGNAL_IRQ_FIRED,
    .transfer      = 1,
    .destroy       = irq_destroy,
};

void devhost_sys_init(void) {
    if (ocache_init(&device_cache, "device", sizeof(struct kdevice)) != 0 ||
        ocache_init(&irq_cache, "interrupt", sizeof(struct kirq)) != 0 ||
        kobj_type_register(KOBJ_DEVICE, &device_ops) != 0 ||
        kobj_type_register(KOBJ_IRQ, &irq_ops) != 0)
        kpanic("devhost_sys_init: no device caches");
}

/* ---- syscalls ----------------------------------------------------------------- */

/* Finding and opening hardware share one gate. */
static int driver_granted(void) {
    if (thread_current_caps() & GRANT_DRIVER) return 1;
    klog_u32("GRANT", SEV_WARN, "driver DENIED for pid ", (uint32_t)thread_current_pid(),
             LOG_COLOR_VALUE, " (no GRANT_DRIVER)");
    return 0;
}

int syscall_device_info(uint32_t id, uint64_t d) {
    if (!driver_granted()) return E_ACCESS_DENIED;
    if (id >= DEVICE_MAX) return E_OUT_OF_RANGE;
    if (!user_ptr_writable(d, sizeof(struct device_desc))) return E_BAD_ADDRESS;
    struct dev_node n;
    if (!devtree_copy(id, &n)) return E_NOT_FOUND;
    struct device_desc di = {
        .id = n.id, .parent = n.parent, .bus = n.bus, .state = n.state,
        .vendor = n.ids.vendor, .device = n.ids.device,
        .class_code = n.ids.cls, .subclass = n.ids.sub, .interface = n.ids.prog,
        .location = n.pci ? ((uint32_t)n.pci->bus << 16 | (uint32_t)n.pci->slot << 8 | n.pci->func) : 0,
    };
    return user_copy_out(d, &di, sizeof di) == 0 ? E_OK : E_BAD_ADDRESS;
}

int syscall_device_open(uint32_t id) {
    if (!driver_granted()) return E_ACCESS_DENIED;
    if (id >= DEVICE_MAX) return E_OUT_OF_RANGE;
    int rc;
    struct dev_node *n = devtree_hold(id, &rc);
    if (!n) return rc;
    struct kdevice *d = ocache_alloc(&device_cache);
    if (!d || kobj_init(&d->hdr, KOBJ_DEVICE) != 0) {
        if (d) ocache_free(&device_cache, d);
        devtree_release(n);
        return d ? E_INTERNAL : E_NO_MEMORY;
    }
    d->node = n;
    d->pci = n->pci;
    d->irq_taken = 0;
    d->retired = 0;
    const struct pci_device *p = d->pci;
    d->command = pci_config_read32(p->bus, p->slot, p->func, 0x04);
    /* The BARs are read now, while nothing drives the device: the size
       probe stops its decoding for a moment. */
    for (int i = 0; i < 6; i++) {
        if (!pci_bar_get(p, i, &d->bar[i])) continue;
        /* the upper half of a 64-bit BAR is no BAR of its own */
        if (d->bar[i].is_64 && i < 5) { d->bar[i + 1] = (struct pci_bar){ 0 }; i++; }
    }
    pci_enable_bus_master(p);
    int32_t h = htab_install(thread_htab(thread_current_id()), &d->hdr,
                             HRIGHT_BAR | HRIGHT_IRQ | HRIGHT_DMA | HRIGHT_DUPLICATE, 0);
    kobj_unref(&d->hdr);   /* the handle's reference keeps it; with no handle, it goes */
    return h;
}

/* The device behind `h`, with a reference, if the handle has `right`. */
static struct kdevice *device_ref(int32_t h, uint32_t right, int *rc) {
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) { *rc = E_BAD_HANDLE; return 0; }
    *rc = o->type != KOBJ_DEVICE    ? E_WRONG_TYPE
        : !(hi.rights & right)      ? E_ACCESS_DENIED : E_OK;
    if (*rc != E_OK) { kobj_unref(o); return 0; }
    return (struct kdevice *)o;
}

int syscall_device_bar(int32_t h, uint32_t bar) {
    int rc;
    struct kdevice *d = device_ref(h, HRIGHT_BAR, &rc);
    if (!d) return rc;
    const struct pci_bar *b = bar < 6 ? &d->bar[bar] : 0;
    rc = !b                                              ? E_OUT_OF_RANGE
       : !b->valid || !b->size                           ? E_NOT_FOUND
       : b->is_io                                        ? E_NOT_SUPPORTED
       : (b->base | b->size) & (PAGE_SIZE - 1u)          ? E_NOT_SUPPORTED : E_OK;
    struct kmem *m = 0;
    if (rc == E_OK) m = kmem_create_device((paddr_t)b->base, b->size / PAGE_SIZE, d, &rc);
    kobj_unref(&d->hdr);
    if (!m) return rc;
    int32_t mh = htab_install(thread_htab(thread_current_id()), &m->hdr,
                              HRIGHT_MAP | HRIGHT_MAP_WRITE | HRIGHT_INSPECT | HRIGHT_DUPLICATE, 0);
    kobj_unref(&m->hdr);
    return mh;
}

int syscall_device_irq(int32_t h) {
    int rc;
    struct kdevice *d = device_ref(h, HRIGHT_IRQ, &rc);
    if (!d) return rc;
    if (!irq_msi_available() || !pci_find_capability(d->pci, PCI_CAP_ID_MSI)) {
        kobj_unref(&d->hdr);
        return E_NOT_SUPPORTED;
    }
    uint32_t none = 0;
    if (!__atomic_compare_exchange_n(&d->irq_taken, &none, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        kobj_unref(&d->hdr);
        return E_BUSY;
    }
    struct kirq *k = ocache_alloc(&irq_cache);
    int vector = k ? irq_alloc_msi_vector(devhost_isr) : -1;
    if (!k || vector < 0 || kobj_init(&k->hdr, KOBJ_IRQ) != 0) {
        if (vector >= 0) irq_free_msi_vector(vector);
        if (k) ocache_free(&irq_cache, k);
        __atomic_store_n(&d->irq_taken, 0, __ATOMIC_RELEASE);
        kobj_unref(&d->hdr);
        return !k ? E_NO_MEMORY : vector < 0 ? E_NO_RESOURCES : E_INTERNAL;
    }
    k->dev = d;            /* the reference device_ref took */
    k->vector = vector;
    k->count = 0;
    uint32_t f = kwait_lock();
    by_vector[vector & 0xFF] = k;
    kwait_unlock(f);
    if (!pci_msi_enable(d->pci, (uint8_t)vector)) {
        kobj_unref(&k->hdr);   /* its destroy undoes the rest */
        return E_NOT_SUPPORTED;
    }
    int32_t ih = htab_install(thread_htab(thread_current_id()), &k->hdr,
                              HRIGHT_WAIT | HRIGHT_ACK | HRIGHT_DUPLICATE, 0);
    kobj_unref(&k->hdr);
    return ih;
}

int syscall_irq_ack(int32_t h) {
    struct htab_info hi;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_NONE, 0, &hi);
    if (!o) return E_BAD_HANDLE;
    int rc = o->type != KOBJ_IRQ        ? E_WRONG_TYPE
           : !(hi.rights & HRIGHT_ACK)  ? E_ACCESS_DENIED : E_OK;
    if (rc == E_OK) {
        struct kirq *k = (struct kirq *)o;
        uint32_t f = kwait_lock();
        uint32_t n = k->count;
        k->count = 0;
        kobj_signal_clear_locked(o, SIGNAL_IRQ_FIRED);
        kwait_unlock(f);
        rc = n > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)n;
    }
    kobj_unref(o);
    return rc;
}

int syscall_dma_alloc(int32_t h, uint64_t size, uint64_t phys) {
    int rc;
    struct kdevice *d = device_ref(h, HRIGHT_DMA, &rc);
    if (!d) return rc;
    rc = size == 0                              ? E_INVALID_ARGS
       : size > DMA_ALLOC_MAX                   ? E_OUT_OF_RANGE
       : !user_ptr_writable(phys, sizeof(uint64_t)) ? E_BAD_ADDRESS : E_OK;
    struct kmem *m = 0;
    if (rc == E_OK)
        m = kmem_create_dma((uint32_t)((size + PAGE_SIZE - 1u) / PAGE_SIZE),
                            thread_proc(thread_current_id())->job, d, &rc);
    kobj_unref(&d->hdr);
    if (!m) return rc;
    uint64_t pa = (uint64_t)m->phys;
    struct htab *tab = thread_htab(thread_current_id());
    int32_t mh = htab_install(tab, &m->hdr,
                              HRIGHT_MAP | HRIGHT_MAP_WRITE | HRIGHT_INSPECT | HRIGHT_DUPLICATE, 0);
    kobj_unref(&m->hdr);
    if (mh > 0 && user_copy_out(phys, &pa, sizeof pa) != 0) {
        htab_close(tab, mh);
        return E_BAD_ADDRESS;
    }
    return mh;
}
