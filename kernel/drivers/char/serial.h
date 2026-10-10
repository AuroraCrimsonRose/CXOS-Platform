// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/char/serial.h */
/* Aurora Tejeda */
/* 16550 UART on COM1, as a write-only debug console.

   The kernel's console is the screen, which cannot be read back by a script:
   verifying a boot otherwise means dumping 0xb8000 through the QEMU monitor
   (text mode only) or reading screenshots (framebuffer). This driver tees the
   same output to a serial port, so `-serial file:boot.log` captures the whole
   boot as text in either video mode.

   Write-only on purpose. Nothing here reads input: a debug log that can block
   on an absent device is worse than no log, and input would need an IRQ path
   this does not have. */

#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include "config.h"

/* Probe COM1 and configure it. Safe to call before any other serial call, and
   safe to call when no UART exists: the probe fails and every later call
   becomes a no-op. Call once, early - before the first console output that
   should appear in the log. */
void serial_init(void);

/* 1 once serial_init() has found a working UART. 0 when the probe failed or
   the driver is compiled out. */
int serial_active(void);

/* Write one byte. No newline translation - the caller decides, because the
   console tee has to emit CR LF without the console itself knowing about
   serial. Spins on the transmitter with a bounded timeout and gives up rather
   than hanging the boot. */
void serial_putc(char c);

/* Write a NUL-terminated string, byte for byte. */
void serial_puts(const char *s);

#endif
