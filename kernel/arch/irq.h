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

struct registers;   /* the interrupted state: arch/<arch>/idt.h */

typedef void (*irq_handler_t)(struct registers *);

/* A legacy interrupt line, numbered from 0. */
void irq_install_handler(int irq, irq_handler_t handler);
void irq_uninstall_handler(int irq);

/* An MSI has no line - the device writes a vector straight to the interrupt
   controller - so vectors come from a pool. Returns the vector to program
   into the device's MSI capability, or -1 if none are free. */
int  irq_alloc_msi_vector(irq_handler_t handler);
void irq_free_msi_vector(int vector);

#endif
