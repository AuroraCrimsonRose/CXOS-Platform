/* /CXLite/kernel/drivers/acpi.h */
/* Aurora Tejeda */
/*
 * Minimal ACPI support: discover the ACPI tables at boot, cache the registers
 * and sleep-state values we need, and provide shutdown (S5) / system-sleep (S1)
 * / reboot via the ACPI reset register.
 *
 * This is NOT a full ACPI/AML implementation - it does a targeted scan of the
 * DSDT for the _S5_ and _S1_ packages to extract their SLP_TYP values, which is
 * enough for power-off and light sleep without a full AML interpreter (a future
 * project). Capabilities are probed ONCE at acpi_init() and cached; the power
 * commands just consult the cached flags.
 */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* probe ACPI tables and cache capabilities. call once at boot. returns 1 if an
   ACPI RSDP/FADT was found, 0 otherwise. */
int acpi_init(void);

/* Find an ACPI table by 4-character signature, fully mapped, or 0. Used by the
   APIC code to reach the MADT, which is the only description of where the
   local and I/O APICs live and how the legacy IRQs are wired to them. */
void *acpi_find_table(const char *sig);

/* capability queries (valid after acpi_init) */
int acpi_can_shutdown(void);   /* S5 SLP_TYP found in DSDT */
int acpi_can_s1(void);         /* S1 SLP_TYP found in DSDT */
int acpi_can_reset(void);      /* FADT provides a reset register */

/* power actions. shutdown/reboot do not return on success. */
void acpi_shutdown(void);      /* enter S5 (soft off) */
int  acpi_sleep_s1(void);      /* enter S1; returns 0 on wake, -1 if unsupported */
int  acpi_reboot(void);        /* via ACPI reset register; returns -1 if unavailable */

#endif