/* /kernel/drivers/char/console.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * Text console for v5: the kernel's low-level output primitives. Tracks the
 * cursor, wraps lines, scrolls, and writes strings/numbers in color, with two
 * interchangeable backends behind one interface - VGA text (default) and the
 * framebuffer (after console_use_fb). No scrollback (v5 is a boot log, not a
 * shell).
 *
 * Higher-level severity/tag logging ("[INFO] ...", "[ERR] ...", child lines)
 * lives in lib/string/logging.h, which builds on these primitives.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include "color.h"

/* initialize: clear the screen, home the cursor, set the default attribute. */
void console_init(void);

/* Switch the console onto the framebuffer, rendering text into the pixel
   rectangle (x0,y0,w,h) with the 8x16 font. No-op unless fb_init() succeeded.
   Lets the boot log occupy part of the screen (e.g. the left side) while the
   rest holds other graphics (e.g. the boot logo). */
void console_use_fb(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h);

/* clear the screen to the current background and home the cursor. */
void console_clear(void);

/* set / get the active text attribute (fg/bg) for subsequent writes. */
void console_set_color(uint8_t attr);
uint8_t console_get_color(void);

/* write a single character, handling \n (newline), \r and \t (tab). */
void console_putc(char c);

/* write a NUL-terminated string at the cursor. */
void console_puts(const char *s);

/* write a string in a specific color (saves/restores the current color). */
void console_puts_color(const char *s, uint8_t attr);

/* write a number at the cursor (decimal / hex). */
void console_put_u32(uint32_t v);
void console_put_i32(int32_t v);
void console_put_hex(uint32_t v, int width);

/* same, but print the number in a specific color (current color restored). */
void console_put_u32_color(uint32_t v, uint8_t attr);
void console_put_i32_color(int32_t v, uint8_t attr);
void console_put_hex_color(uint32_t v, int width, uint8_t attr);

/* label + value in one call: print `label` in the current color, then the
   number in `value_attr` (current color restored after). */
void console_field_u32(const char *label, uint32_t v, uint8_t value_attr);
void console_field_i32(const char *label, int32_t v, uint8_t value_attr);
void console_field_hex(const char *label, uint32_t v, int width, uint8_t value_attr);

/* line / cursor control */
void console_newline(void);
void console_set_cursor(int x, int y);
void console_get_cursor(int *x, int *y);

#endif