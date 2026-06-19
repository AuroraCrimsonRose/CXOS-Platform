/* /CXK/kernel/drivers/char/console.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * Minimal text-mode console for v5. Stateful layer on top of vga.c: tracks the
 * cursor, wraps lines, scrolls, and writes strings/numbers in color. This is
 * the kernel's normal output path (replacing kmain's hand-placed VGA pokes).
 *
 * Deliberately small - no scrollback buffer, no framebuffer backend (the heavy
 * v4 console had both; v5 doesn't need them without a shell). A framebuffer
 * backend can be added later behind this same interface.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include "color.h"

/* initialize: clear the screen, home the cursor, set the default attribute. */
void console_init(void);

/* clear the screen to the current background and home the cursor. */
void console_clear(void);

/* set / get the active text attribute (fg/bg) for subsequent writes. */
void console_set_color(uint8_t attr);
uint8_t console_get_color(void);

/* write a single character, handling \n (newline) and \t (tab). */
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
   number in `value_attr` (current color restored after). The common boot-log
   shape, e.g. console_field_u32("free: ", n, white). */
void console_field_u32(const char *label, uint32_t v, uint8_t value_attr);
void console_field_i32(const char *label, int32_t v, uint8_t value_attr);
void console_field_hex(const char *label, uint32_t v, int width, uint8_t value_attr);

/* line control */
void console_newline(void);
void console_set_cursor(int x, int y);
void console_get_cursor(int *x, int *y);

/* tagged log lines: print "[TAG] message\n", tag colorized. */
void console_tag(const char *tag, uint8_t tag_attr, const char *msg);
void console_boot(const char *msg);     /* "[BOOT] msg"   (cyan)   */
void console_kernel(const char *msg);   /* "[KERNEL] msg" (green)  */
void console_warn(const char *msg);     /* "[WARN] msg"   (yellow) */
void console_err(const char *msg);      /* "[ERR] msg"    (red)    */

#endif