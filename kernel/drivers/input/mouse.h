// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/input/mouse.h */
/* Aurora Tejeda / CATX Systems */
/* PS/2 mouse: absolute cursor position + button state, driven by IRQ12. */

#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* Initialise the mouse and clamp the cursor to a screen of this size.
   Returns 1 if a mouse responded, 0 if not. Call after the IDT/PIC are up. */
int mouse_init(int screen_w, int screen_h);

int      mouse_present(void);
int32_t  mouse_x(void);
int32_t  mouse_y(void);
uint32_t mouse_buttons(void);   /* bit 0 left, bit 1 right, bit 2 middle */

/* Bumped on every state change - poll this to know if anything moved without
   comparing coordinates. */
uint32_t mouse_seq(void);

/* Re-clamp when the display mode changes. */
void mouse_set_bounds(int w, int h);

/* Apply a movement from outside the IRQ12 path, in SCREEN direction - dy
   positive means downward. USB HID uses this; the PS/2 handler negates its own
   dy on the way in, because PS/2 reports Y positive-up and USB does not. */
void mouse_inject(int32_t dx, int32_t dy, uint32_t btns);

#endif
