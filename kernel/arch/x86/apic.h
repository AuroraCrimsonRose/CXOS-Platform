// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86/apic.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Local APIC and I/O APIC - the modern interrupt path, replacing the 8259 pair.
 *
 * ---- Why bother, when the PIC works -------------------------------------
 *
 * Three things the 8259 cannot do, in the order they matter here:
 *
 *   1. MSI. A device signals an MSI by WRITING TO MEMORY at 0xFEE.....  - the
 *      local APIC's address. There is no wire and no 8259 involvement at all,
 *      so a machine without a local APIC cannot receive one. This is the
 *      immediate reason: MSI needs only the LAPIC, not the I/O APIC.
 *   2. More than 15 interrupt lines, and sane sharing.
 *   3. IPIs, without which the other cores of an FX chip can never be started.
 *
 * ---- What this does and does not change ----------------------------------
 *
 * irq_install_handler() keeps working exactly as before. Drivers are unaware:
 * the same handler is called for the same IRQ number. What changes underneath
 * is who delivers the interrupt (I/O APIC rather than 8259) and who is
 * acknowledged afterwards (local APIC rather than 8259).
 *
 * If anything is missing - no CPUID APIC bit, no MADT, no I/O APIC entry - the
 * whole thing declines and the 8259 keeps running. A machine that boots today
 * must still boot.
 */

#ifndef APIC_H
#define APIC_H

#include <stdint.h>
#include "arch/irq.h"   /* irq_controller_init, irq_msi_*, irq_mask/unmask */

int apic_active(void);        /* are IRQs coming from the I/O APIC?     */
int lapic_active(void);       /* is the local APIC enabled? MSI needs this */

/* Acknowledge the interrupt currently being serviced. */
void lapic_eoi(void);

uint32_t lapic_id(void);      /* this CPU's local APIC id - MSI destination */

/* The local APIC timer's vector, the first past the MSI pool (idt.h), and its
   interrupt, which the IDT's dispatch calls before the EOI. The timer itself
   is arch/evtimer.h's. */
#define LAPIC_TIMER_VECTOR 56
void lapic_timer_interrupt(struct registers *r);

/* 1.9d: the IPI that wakes a parked CPU (arch/x86_64/smp.c), the next
   vector, and what starting one needs: an INIT and a STARTUP aimed at one
   local APIC ID (never a broadcast: the MADT says which exist), a fixed IPI,
   and the AP's own local APIC brought up as the boot CPU's was, its timer
   set up but not counting. Each returns 0, or -1 if the IPI was never
   accepted (the delivery-status bit never cleared). */
#define SMP_IPI_VECTOR 57
int  lapic_send_init(uint32_t apic_id);
int  lapic_send_startup(uint32_t apic_id, uint32_t page);
int  lapic_send_fixed(uint32_t apic_id, uint8_t vector);
void lapic_ap_init(void);
uint32_t lapic_svr(void);
uint32_t lapic_tpr(void);
uint32_t lapic_lvt_timer(void);
void lapic_timer_oneshot(uint32_t count, int unmask);

/* The MADT's enabled processors, in its order (arch/x86/madt.h), read by
   irq_controller_init. */
struct madt_cpu;
struct madt_cpu_counts;
uint32_t apic_cpus(const struct madt_cpu **list, const struct madt_cpu_counts **counts);


#endif
