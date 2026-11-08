// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/devhost.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The driver host's primitives (Phase 2.2, kernel/CX_DRIVERS.md §3.2, §4
 * step 3): what a driver in a process is given, and nothing more.
 *
 *   - A DEVICE (KOBJ_DEVICE, SYS_DEVICE_OPEN): one PCI function no kernel
 *     driver has, held (DEV_HELD in the device tree) for as long as anything
 *     refers to it. Opening it needs GRANT_DRIVER; everything after needs
 *     only the handle's rights, so a supervisor opens and a driver drives.
 *   - Its REGISTERS as memory objects (SYS_DEVICE_BAR): KMEM_DEVICE, one per
 *     memory BAR, mapped uncached by SYS_VM_MAP. Never arbitrary physical
 *     memory: the range is the BAR's, read from the device when it was
 *     opened (Redox's physmap, which maps any address, is the trap
 *     CX_DRIVERS.md §2 names).
 *   - Its INTERRUPT as a handle (KOBJ_IRQ, SYS_DEVICE_IRQ): an MSI vector
 *     whose interrupt sets SIGNAL_IRQ_FIRED, so the driver waits for it as
 *     for anything else, alone or in a queue; SYS_IRQ_ACK counts and clears.
 *     MSI only: MSI-X's table lies in a BAR the driver maps, and the line
 *     interrupts need the routing ACPI's AML gives (CX_DRIVERS.md §5).
 *   - DMA BUFFERS (SYS_DMA_ALLOC): KMEM_DMA, one run below 4 GB, with its
 *     physical address. A buffer's last reference does not free its pages
 *     while the device is open: the device may still be writing them, and
 *     with no IOMMU nothing stops it. The buffer is RETIRED to its device
 *     instead, and its pages go back only after the device's bus mastering
 *     is off (Fuchsia's bus transaction initiator quarantines pinned pages
 *     the same way).
 *
 * The order a device ends in is the reason for the references: each BAR
 * object, the interrupt and each DMA buffer hold one on the device, so the
 * device's destroy runs last, when nothing of the driver's can reach it.
 * Then bus mastering goes off, the retired buffers are freed, and the node
 * is DEV_UNBOUND again.
 *
 * Locking: the vector table is under the wait lock (object/kwait.h), which
 * the interrupt takes to signal; the retired list is a lock-free stack
 * (pushed by a buffer's destroy, from wherever its last reference went, and
 * emptied only by the device's own destroy, when no buffer can still push).
 */
#ifndef CXK_DEVHOST_H
#define CXK_DEVHOST_H

#include <stdint.h>
#include "object/kobj.h"
#include "drivers/bus/pci.h"

struct dev_node;
struct kmem;

struct kdevice {
    struct kobj hdr;              /* first; KOBJ_DEVICE */
    struct dev_node *node;        /* DEV_HELD while this lives */
    const struct pci_device *pci;
    struct pci_bar bar[6];        /* read at open; valid 0 for none, or a 64-bit BAR's upper half */
    uint32_t command;             /* the command register before the open */
    uint32_t irq_taken;           /* 1 while an interrupt handle's object lives */
    struct kmem *retired;         /* DMA buffers whose last reference went */
};

/* The types' registration, once, at boot (proc_sys_init). */
void devhost_sys_init(void);

/* References on a device: each memory object over it holds one. */
void devhost_get(struct kdevice *d);
void devhost_put(struct kdevice *d);

/* A DMA buffer's last reference has gone (object/kmem.c): its pages wait for
   the device to close. Drops the buffer's reference on the device, which may
   be the last. */
void devhost_dma_retire(struct kdevice *d, struct kmem *m);

/* For the self-tests: the buffers retired to `d` and not yet freed. */
uint32_t devhost_retired(const struct kdevice *d);

#endif
