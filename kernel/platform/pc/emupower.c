// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/emupower.c */
/* Aurora Tejeda / CATX Systems */

#include "emupower.h"
#include "drivers/iowin.h"

/* the emulators' fixed ACPI PM1a control registers */
static struct iowin qemu_pm  = IOWIN_PORTS(0x604, 2);
static struct iowin bochs_pm = IOWIN_PORTS(0xB004, 2);

void emu_poweroff(void) {
    iowin_write16(&qemu_pm, 0, 0x2000);    /* QEMU: SLP_EN with S5 */
    iowin_write16(&bochs_pm, 0, 0x2000);   /* Bochs / older QEMU */
}
