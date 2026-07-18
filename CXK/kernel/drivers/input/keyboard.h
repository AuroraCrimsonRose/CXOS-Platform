/* /CXLite/kernel/drivers/keyboard.h */
/* Aurora Tejeda */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* install the IRQ1 handler and initialize state */
void keyboard_init(void);

/* return the next character typed, or 0 if none available */
char keyboard_getchar(void);

/* block until a character is available, then return it */
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