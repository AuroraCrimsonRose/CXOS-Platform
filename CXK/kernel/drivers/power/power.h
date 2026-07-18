/* /CXK/kernel/drivers/power/power.h */
/* Aurora Tejeda / CATX Systems LLC */
/* Power management (minimal). Reboot is real (8042 pulse + triple-fault
   fallback). Shutdown/sleep can be ported from v4 when ACPI lands. */

#ifndef POWER_H
#define POWER_H

#include <stdint.h>

/* Reboot the machine. Pulses the 8042 keyboard controller, with a triple-fault
   fallback. Does not return. */
void power_reboot(void);

#endif
