// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/keyboard.h */
/* Aurora Tejeda */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* return the next character typed, or 0 if none available */
char keyboard_getchar(void);

/* Push a key into the input buffer. Every keyboard driver does: PS/2
   (platform/pc/ps2kbd.c, from IRQ 1) and USB HID alike, so nothing above here
   can tell them apart. Safe from an interrupt handler. */
void keyboard_inject(unsigned char c);

/* block until a character is available, then return it. A USB keyboard
   raises no IRQ 1, so with a HID device the wait also ends every 10 ms to
   poll it. */
char keyboard_getchar_blocking(void);

/* Special (non-ASCII) key codes returned by keyboard_getchar(). These are
   values above the ASCII range so they never collide with printable input.
   The shell's line editor checks for these; code that only handles printable
   characters simply won't match them. */
#define KEY_UP        0x80
#define KEY_DOWN      0x81
#define KEY_LEFT      0x82
#define KEY_RIGHT     0x83
#define KEY_PGUP      0x84
#define KEY_PGDN      0x85
#define KEY_SHIFT_UP  0x86   /* Shift+Up - scroll back one line */
#define KEY_SHIFT_DN  0x87   /* Shift+Down - scroll forward one line */
#define KEY_HOME      0x88   /* jump to start of line */
#define KEY_END       0x89   /* jump to end of line */
#define KEY_CTRL_C    0x03   /* Ctrl+C (ETX) - used as "abort" */

#endif