/* /CXLite/kernel/drivers/fb.h */
/* Aurora Tejeda */
/*
 * Linear framebuffer driver (VBE graphics mode).
 *
 * The bootloader (vbe.asm) sets a VBE mode in real mode and stashes the
 * framebuffer details at fixed low-memory addresses. fb_init() reads them.
 * If the bootloader couldn't set a graphics mode (VBE_VALID == 0), fb_init()
 * returns -1 and the kernel stays in VGA text mode.
 *
 * Supports 32bpp (0x00RRGGBB) and 16bpp (5-6-5 RGB) direct-color modes.
 */

#ifndef FB_H
#define FB_H

#include <stdint.h>

/* try to initialize from the bootloader's VBE info.
   returns 0 if a framebuffer is active, -1 if none (stay in text mode). */
int fb_init(void);

/* is a graphics framebuffer active? */
int fb_active(void);

/* physical address + byte size of the framebuffer (for the pager to map).
   returns 0 and zeroes the outputs if no framebuffer is active. */
int fb_get_region(uint32_t *phys, uint32_t *size);

/* geometry */
uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_bpp(void);

/* pack an 8-bit-per-channel RGB into the active mode's pixel format */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

/* drawing primitives (color is a value from fb_rgb) */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_clear(uint32_t color);

/* draw a line between two points (Bresenham, integer). */
void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color);

/* scroll the framebuffer up by `pixels` rows, filling the bottom with `bg`.
   used by the console for fast scrolling instead of full re-render. */
void fb_scroll_up(uint32_t pixels, uint32_t bg);

/* text rendering using the built-in fonts (both are 8 px wide; the height
   differs: the 8x16 font is taller, the 8x8 font fits twice as many rows). */
#define FB_CHAR_W 8

enum fb_font {
    FB_FONT_8X16 = 0,   /* default - taller */
    FB_FONT_8X8         /* compact - half height */
};

/* select the active font for subsequent text drawing. */
void fb_set_font(enum fb_font f);
/* current glyph height in pixels (16 or 8); width is always FB_CHAR_W. */
uint32_t fb_font_height(void);

void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void fb_draw_string(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg);

#endif