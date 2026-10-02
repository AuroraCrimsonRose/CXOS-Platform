// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/vga.h */
/* Aurora Tejeda */
/* Text-mode VGA renderer: low-level cell drawing + hardware cursor.
   Stateless. The console driver (console.c) sits on top of this. */

#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include "color.h"   /* enum vga_color, VGA_ATTR, vga_attr() live here now */

#define VGA_WIDTH   80
#define VGA_HEIGHT  25

/* draw one character cell at (x, y) with the given attribute */
void vga_put_cell(int x, int y, char c, uint8_t attr);

/* fill the whole screen with spaces of the given attribute */
void vga_clear(uint8_t attr);

/* move the blinking hardware cursor to (x, y) */
void vga_set_cursor(int x, int y);

/* hide / show the hardware cursor */
void vga_disable_cursor(void);
void vga_enable_cursor(void);

#endif