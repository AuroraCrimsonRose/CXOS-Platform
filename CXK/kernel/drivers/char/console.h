/* /CXLite/kernel/drivers/console.h */
/* Aurora Tejeda */
/* Console driver: cursor tracking, scrolling, color, line handling.
   The front door for all text output. Renders via vga.c. */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

/* initialize console: clear screen, reset cursor, default colors */
void console_init(void);

/* clear the screen and home the cursor */
void console_clear(void);

/* set the current text color (foreground, background) for future output */
void console_set_color(uint8_t fg, uint8_t bg);

/* print one character (handles \n, \b, \t, scrolling) */
void console_putc(char c);

/* print a null-terminated string */
void console_print(const char *s);

/* batch multi-line output: wrap a command's output in begin/end to render once
   at the end (one screen redraw) instead of scrolling per line. Nestable. */
void console_begin_batch(void);
void console_end_batch(void);

/* print a 32-bit value as hexadecimal (0x........) */
void console_print_hex(uint32_t v);

/* print a 32-bit unsigned value in decimal */
void console_print_dec(uint32_t v);

/* scrollback controls */
void console_scroll_up(int lines);    /* scroll view up into older history */
void console_scroll_down(int lines);  /* scroll view back toward the present */
void console_scroll_reset(void);      /* snap back to the live bottom */

/* switch graphics-mode font: 1 = small (8x8), 0 = large (8x16).
   no-op in VGA text mode. recomputes geometry and re-renders. */
void console_set_font(int small);

/* set the cursor to a specific column on the current line (for line editing) */
void console_set_cursor_col(int col);

#endif