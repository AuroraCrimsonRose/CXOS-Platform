/* /CXK/kernel/drivers/video/fb.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * Linear framebuffer driver (VBE graphics mode).
 *
 * The bootloader (boot/vbe.asm) sets a VBE linear-framebuffer mode in real mode
 * and stashes the mode details at a fixed low-memory struct (physical 0x1C40).
 * fb_init() reads them (via the kernel's higher-half window), maps the LFB into
 * kernel-virtual space itself, and is then ready to draw. If the bootloader
 * couldn't set a graphics mode (VBE_VALID == 0), fb_init() returns -1 and the
 * kernel stays in VGA text mode.
 *
 * 16bpp (5-6-5) and 32bpp (0x00RRGGBB) direct-color modes are supported; the
 * bootloader currently only ever selects 16bpp, but the 32bpp path is kept for
 * when a future mode list offers it.
 *
 * Text uses the built-in 8x16 font only. (The font lib still carries an 8x8
 * table, but the framebuffer console no longer uses it - small glyphs are hard
 * to read and one font keeps the layout math simple.)
 */

#ifndef FB_H
#define FB_H

#include <stdint.h>

/* Try to initialize from the bootloader's VBE info and map the framebuffer.
   Must be called AFTER paging + PMM are up (it maps the LFB via paging_map).
   Returns 0 if a framebuffer is active, -1 if none (stay in text mode). */
int fb_init(void);

/* Is a graphics framebuffer active? */
int fb_active(void);

/* Physical address + byte size of the framebuffer (informational; the driver
   maps the LFB itself in fb_init). Returns 0 and zeroes the outputs if none. */
int fb_get_region(uint32_t *phys, uint32_t *size);

/* Geometry. */
uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_bpp(void);

/* Pack an 8-bit-per-channel RGB into the active mode's pixel format. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Drawing primitives (color is a value from fb_rgb). */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_clear(uint32_t color);

/* Line between two points (Bresenham, integer). */
void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color);

/* Scroll the framebuffer up by `pixels` rows, filling the bottom with `bg`.
   Used by the console for fast scrolling instead of a full re-render. */
void fb_scroll_up(uint32_t pixels, uint32_t bg);

/* Text rendering using the built-in 8x16 font. Width is always FB_CHAR_W. */
#define FB_CHAR_W   8
#define FB_CHAR_H   16

/* Glyph height in pixels (always 16). Kept as a function so console code reads
   it the same way it would a runtime-selectable value. */
uint32_t fb_font_height(void);

void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void fb_draw_string(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg);

#endif