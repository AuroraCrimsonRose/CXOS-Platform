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
    uint8_t shift, caps, ctrl, ext;
};

/* Feed one scancode byte (set 1). Returns the key it completes - an ASCII
   character, a control code (Ctrl+A = 1 .. Ctrl+Z = 26) or a KEY_* value from
   keyboard.h - or 0 if it completes none (a release, a modifier, a prefix). */
unsigned ps2kbd_decode(struct ps2kbd_state *s, uint8_t sc);

/* Take IRQ 1 and feed every scancode to keyboard.c. */
void ps2kbd_init(void);

#endif
