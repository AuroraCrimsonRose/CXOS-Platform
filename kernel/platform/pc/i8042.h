// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/i8042.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The 8042 PS/2 controller: one chip behind the keyboard (IRQ 1), the mouse
 * (IRQ 12) and, for historical reasons, the CPU's reset line. This file owns
 * its two ports; ps2kbd.c, ps2mouse.c and the reboot path go through it.
 */

#ifndef PLATFORM_PC_I8042_H
#define PLATFORM_PC_I8042_H

#include <stdint.h>

/* status register bits */
#define I8042_STAT_OUTPUT_FULL 0x01   /* a byte waits in the data port */
#define I8042_STAT_INPUT_FULL  0x02   /* the controller has not taken the last write */
#define I8042_STAT_FROM_AUX    0x20   /* the waiting byte came from the mouse */

/* controller commands */
#define I8042_CMD_READ_CFG     0x20
#define I8042_CMD_WRITE_CFG    0x60
#define I8042_CMD_ENABLE_AUX   0xA8   /* enable the second (mouse) port */
#define I8042_CMD_WRITE_KBD_OUT 0xD2  /* the next data byte comes back as if from the keyboard */
#define I8042_CMD_WRITE_AUX_OUT 0xD3  /* ... or from the mouse */
#define I8042_CMD_TO_AUX       0xD4   /* the next data byte goes to the mouse */
#define I8042_CMD_PULSE_RESET  0xFE   /* pulse the CPU reset line */

uint8_t i8042_status(void);

/* The data port as it is, no waiting: for an IRQ handler, whose interrupt
   already says a byte is there. */
uint8_t i8042_data(void);

/* Wait (bounded) until the controller can take a byte, then write it to the
   command port or the data port. */
void i8042_command(uint8_t cmd);
void i8042_write(uint8_t v);

/* Wait (bounded) for a byte, then read it. Gives whatever the port holds if
   none came. */
uint8_t i8042_read(void);

/* Drain the controller, then pulse the CPU reset line. Returns only if the
   machine did not reset (no 8042, or one that ignores the command). */
void i8042_pulse_reset(void);

#endif
