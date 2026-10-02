/* /kernel/cpu/int/apic.h */
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

/* Bring up the local APIC, and the I/O APIC if the MADT describes one. Returns
   1 if interrupts are now delivered through the APICs, 0 if the 8259 is still
   in charge. Call after acpi_init(). */
int apic_init(void);

int apic_active(void);        /* are IRQs coming from the I/O APIC?     */
int lapic_active(void);       /* is the local APIC enabled? MSI needs this */

/* Acknowledge the interrupt currently being serviced. */
void lapic_eoi(void);

uint32_t lapic_id(void);      /* this CPU's local APIC id - MSI destination */

/* Physical address a device writes to signal an MSI, and the data value that
   selects `vector`. Both are architectural on x86 rather than device-specific. */
uint32_t msi_message_address(void);
uint32_t msi_message_data(uint8_t vector);

/* Mask or unmask a legacy IRQ at whichever controller is live. */
void irq_mask(int irq);
void irq_unmask(int irq);

#endif
