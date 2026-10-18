// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/platform.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The machine around the CPU (docs/kernel/CX_KERNEL_PLAN.md section 2.2).
 *
 * A platform's fixed devices - the tick source, the reset and poweroff lines,
 * the battery-backed clock, the boot text screen, the built-in keyboard and
 * mouse - are reached from generic code only through these, implemented once
 * per platform (kernel/platform/pc/platform.c on a PC). Devices found by
 * enumeration (PCI, ACPI) are not here: their drivers are generic. Only
 * kernel/arch/ and kernel/platform/ may include a platform's own headers
 * (kernel/platform/<name>/), which the build checks.
 */

#ifndef PLATFORM_PLATFORM_H
#define PLATFORM_PLATFORM_H

#include <stdint.h>
#include "arch/irq.h"   /* irq_handler_t */
#include "datetime.h"

/* First call after the move onto thread 0's stack: the devices that need no
   interrupts and no allocation. */
void platform_init(void);

/* Start the periodic tick: on_tick runs `hz` times a second, as an interrupt. */
void platform_tick_start(uint32_t hz, irq_handler_t on_tick);

/* Start the built-in keyboard and mouse, reporting through keyboard.h and
   mouse.h, and log what was found. Needs interrupts on. */
void platform_input_init(void);

/* Reset the machine through the platform's reset line. Returns if it had no
   effect, so the caller can try the processor's own reset. */
void platform_reset(void);

/* Power off through a fixed mechanism the platform knows without ACPI (an
   emulator's port, say). Returns if it had no effect. */
void platform_poweroff(void);

/* Read the battery-backed wall clock. Returns 0, or -1 with *out zeroed if
   there is none. */
int platform_wallclock(struct datetime *out);

/* The boot text screen: a grid of cells of a character and a colour
   attribute (color.h), drawn before any framebuffer is up. */
int  platform_text_cols(void);
int  platform_text_rows(void);
void platform_text_put(int x, int y, char c, uint8_t attr);
void platform_text_clear(uint8_t attr);
void platform_text_scroll(uint8_t attr);   /* up one row, the new row blank */
void platform_text_cursor(int x, int y);

#endif
