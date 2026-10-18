// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/emupower.h */
/* Aurora Tejeda / CATX Systems */

#ifndef PLATFORM_PC_EMUPOWER_H
#define PLATFORM_PC_EMUPOWER_H

/* Power off through the PM1a control registers QEMU and Bochs keep at fixed
   ports, for when ACPI could not. No effect on real hardware; returns if
   nothing happened. */
void emu_poweroff(void);

#endif
