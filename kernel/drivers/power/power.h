// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/power/power.h */
/* Aurora Tejeda */
/* Power management API.
 *
 * PLACEHOLDER: reboot is real; shutdown and sleep are stubs until ACPI
 * is implemented. The API is stable — when ACPI lands, the guts of
 * power_shutdown()/power_sleep() get filled in and callers don't change.
 */

#ifndef POWER_H
#define POWER_H

#include <stdint.h>

/* Reboot the machine. Real: pulses the keyboard controller, with a
   triple-fault fallback. Does not return. */
void power_reboot(void);

/* Shut down. PLACEHOLDER: real ACPI poweroff is not yet implemented,
   so this halts the CPU with a "safe to power off" message. The machine
   stays powered but frozen. Does not return. */
void power_shutdown(void);

/* Sleep / suspend. PLACEHOLDER: real ACPI sleep (S1-S3) not implemented.
   For now this just idles the CPU until the next interrupt and returns. */
void power_sleep(uint32_t ms);  /* ms=0: S1-or-C1 until keypress; ms>0: C1 timed idle */

#endif