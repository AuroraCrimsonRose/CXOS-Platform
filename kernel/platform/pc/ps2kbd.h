// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/ps2kbd.h */
/* Aurora Tejeda / CATX Systems */

#ifndef PLATFORM_PC_PS2KBD_H
#define PLATFORM_PC_PS2KBD_H

#include <stdint.h>

/* What the scancode stream has said so far: held modifiers, and whether the
   last byte was the 0xE0 prefix of an extended key. */
struct ps2kbd_state {
    uint8_t ext;   /* the last byte was 0xE0 */
    uint8_t e1;    /* bytes of a Pause sequence still to come */
};

/* Feed one scancode byte (set 1). Returns 1 when it completes a key going
   down or up, with the key as a USB HID usage (KC_*) in *usage, else 0 (a
   prefix, a fake shift, a code no key has). What the key means, modifiers
   included, is drivers/input/keymap.c's. */
int ps2kbd_decode(struct ps2kbd_state *s, uint8_t sc, uint32_t *usage, int *down);

/* Take IRQ 1 and feed every scancode to keyboard.c. */
void ps2kbd_init(void);

#endif
