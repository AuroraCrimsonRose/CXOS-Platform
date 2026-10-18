// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/input/mouse.c */
/* Aurora Tejeda / CATX Systems */
/*
 * The pointer as the rest of the kernel sees it: an absolute position clamped
 * to the screen, and the buttons. Whichever mouse is attached
 * (platform/pc/ps2mouse.c, usb_hid.c) reports relative movement through
 * mouse_inject.
 */

#include <stdint.h>
#include "mouse.h"

static int      present;
static int32_t  mx, my;                 /* absolute position, clamped */
static uint32_t buttons;                /* bit 0 left, 1 right, 2 middle */
static int32_t  clamp_w = 640, clamp_h = 480;
static uint32_t moved;                  /* bumped on every state change */

void mouse_inject(int32_t dx, int32_t dy, uint32_t btns) {
    mx += dx;
    my += dy;

    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > clamp_w - 1) mx = clamp_w - 1;
    if (my > clamp_h - 1) my = clamp_h - 1;

    buttons = btns;
    moved++;
    /* A device that is sending movement demonstrably exists, even if the PS/2
       probe found nothing - otherwise a USB-only mouse reports as absent and
       the GUI ignores it. */
    present = 1;
}

void mouse_attach(void) { present = 1; }

/* ---- public ---- */

void mouse_init(int screen_w, int screen_h) {
    if (screen_w  > 0) clamp_w = screen_w;
    if (screen_h > 0) clamp_h = screen_h;
    mx = clamp_w / 2;
    my = clamp_h / 2;
}

int      mouse_present(void)  { return present; }
int32_t  mouse_x(void)        { return mx; }
int32_t  mouse_y(void)        { return my; }
uint32_t mouse_buttons(void)  { return buttons; }
uint32_t mouse_seq(void)      { return moved; }

void mouse_set_bounds(int w, int h) {
    if (w > 0) clamp_w = w;
    if (h > 0) clamp_h = h;
    if (mx > clamp_w - 1) mx = clamp_w - 1;
    if (my > clamp_h - 1) my = clamp_h - 1;
}
