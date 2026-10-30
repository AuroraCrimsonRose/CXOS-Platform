// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/irq.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Interrupt handlers, as generic code sees them. Every architecture provides
 * these; the register frame a handler is given is the architecture's own, and
 * code outside kernel/arch/ passes it on without looking inside.
 */

#ifndef ARCH_IRQ_H
#define ARCH_IRQ_H

#include <stdint.h>

struct registers;   /* the interrupted state: arch/<arch>/idt.h */

typedef void (*irq_handler_t)(struct registers *);

/* A legacy interrupt line, numbered from 0. */
void irq_install_handler(int irq, irq_handler_t handler);
void irq_uninstall_handler(int irq);

/* Mask or unmask a legacy line at whichever controller is live. */
void irq_mask(int irq);
void irq_unmask(int irq);

/* Bring up the interrupt controllers beyond the legacy ones (x86: the local
   APIC, and the I/O APIC if the MADT describes one). Call after acpi_init()
   and before any driver. Returns 1 if they took over; 0 leaves the legacy
   controller in charge, which still works. */
int irq_controller_init(void);

/* An MSI has no line - the device writes a vector straight to the interrupt
   controller - so vectors come from a pool. Returns the vector to program
   into the device's MSI capability, or -1 if none are free. */
int  irq_alloc_msi_vector(irq_handler_t handler);
void irq_free_msi_vector(int vector);

/* Whether an MSI can be delivered at all (x86: the local APIC is running). */
int irq_msi_available(void);

/* The address a device writes to raise an MSI, and the data word that selects
   `vector`. Set by the architecture, not by the device. */
uint32_t irq_msi_address(void);
uint32_t irq_msi_data(uint8_t vector);

/* A fault taken in user mode goes to this hook instead of to a panic, so the
   process can be ended. */
void set_user_fault_hook(void (*hook)(struct registers *));

/* An interrupt taken in user mode calls this once it has been handled (and
   any switch it caused has come back), just before the return to user mode:
   the safe point where a process being killed leaves. */
void set_user_irq_return_hook(void (*hook)(void));

/* A page fault on a user page that was not present, from ring 3 or from a
   user copy, asks this hook first: nonzero if it mapped the page (a first
   touch, memman/vmregion.h's vm_fault), and the instruction runs again. */
void set_demand_fault_hook(int (*hook)(uintptr_t addr, int write, int exec));

/* Which exception or interrupt a frame records, for a log line. */
uint32_t irq_frame_vector(const struct registers *r);

/* The faulting instruction, the error code the CPU pushed (0 when it pushes
   none), and the address a page fault was taken on - for a log line. The last
   is read from the CPU, so it is only meaningful inside the fault's handler. */
uintptr_t irq_frame_ip(const struct registers *r);
uint32_t  irq_frame_error(const struct registers *r);
uintptr_t irq_fault_address(void);

#endif
