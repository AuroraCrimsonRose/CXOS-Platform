/* /CXLite/kernel/drivers/vga.h */
/* Aurora Tejeda */
/* Text-mode VGA renderer: low-level cell drawing + hardware cursor.
   Stateless. The console driver (console.c) sits on top of this. */

#ifndef VGA_H
#define VGA_H

#include <stdint.h>

#define VGA_WIDTH   80
#define VGA_HEIGHT  25

/* VGA text attribute = (bg << 4) | fg  */
#define VGA_ATTR(fg, bg)  ((uint8_t)(((bg) << 4) | ((fg) & 0x0F)))

/* 16 text-mode colors */
enum vga_color {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN,
    VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LIGHT_GREY,
    VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
    VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_YELLOW, VGA_WHITE
};

/* draw one character cell at (x, y) with the given attribute */
void vga_put_cell(int x, int y, char c, uint8_t attr);

/* fill the whole screen with spaces of the given attribute */
void vga_clear(uint8_t attr);

/* move the blinking hardware cursor to (x, y) */
void vga_set_cursor(int x, int y);

/* hide / show the hardware cursor */
void vga_disable_cursor(void);
void vga_enable_cursor(void);

#endif