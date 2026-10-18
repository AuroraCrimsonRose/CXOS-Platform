// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/ps2mouse.h */
/* Aurora Tejeda / CATX Systems */

#ifndef PLATFORM_PC_PS2MOUSE_H
#define PLATFORM_PC_PS2MOUSE_H

/* Probe for a PS/2 mouse and, if one answers, take IRQ 12 and feed its
   movement to mouse.c. Returns 1 if a mouse responded, 0 if not. Call after
   mouse_init, with the IDT and the PIC up. */
int ps2mouse_init(void);

#endif
