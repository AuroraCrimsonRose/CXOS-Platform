// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/input/mouse.h */
/* Aurora Tejeda / CATX Systems */
/* The pointer: absolute cursor position + button state, fed by whichever
   mouse driver is attached (platform/pc/ps2mouse.c, usb_hid.c). */

#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* Clamp the cursor to a screen of this size (a zero keeps 640x480) and centre
   it. Call before any mouse driver starts. */
void mouse_init(int screen_w, int screen_h);

/* A mouse driver found its device. (Movement through mouse_inject says the
   same thing; this is for a driver that probes before anything moves.) */
void mouse_attach(void);

int      mouse_present(void);
int32_t  mouse_x(void);
int32_t  mouse_y(void);
uint32_t mouse_buttons(void);   /* bit 0 left, bit 1 right, bit 2 middle */

/* Bumped on every state change - poll this to know if anything moved without
   comparing coordinates. */
uint32_t mouse_seq(void);

/* Re-clamp when the display mode changes. */
void mouse_set_bounds(int w, int h);

/* Apply a movement, in SCREEN direction - dy positive means downward. Every
   mouse driver reports through this; the PS/2 one negates its own dy on the
   way in, because PS/2 reports Y positive-up and USB does not. */
void mouse_inject(int32_t dx, int32_t dy, uint32_t btns);

#endif
