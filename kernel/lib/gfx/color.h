// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/color.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Color model. Pure - just constants and value packing, no hardware - so it
 * lives in lib/. This is the single home for "what a color is" in CXK, so the
 * text console and (later) a framebuffer console share one definition instead
 * of each inventing their own.
 *
 * TODAY: the 16-color VGA text-mode model (4-bit foreground + 4-bit background,
 * packed into an 8-bit attribute byte). That's the only display CXK drives now.
 *
 * LATER (framebuffer): 8-bit palettized and 32-bit true-color RGB will be added
 * HERE, alongside the VGA model - see the marked seam at the bottom. They're not
 * built yet because nothing renders them; they slot in when the framebuffer
 * lands and gives them a consumer to test against.
 */

#ifndef COLOR_H
#define COLOR_H

#include <stdint.h>

/* ---- 16-color VGA text palette (the standard IBM CGA/VGA order) ---- */
enum vga_color {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN,
    VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LIGHT_GREY,
    VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
    VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_YELLOW, VGA_WHITE
};

/* a VGA text attribute byte = (background << 4) | (foreground & 0x0F) */
static inline uint8_t vga_attr(uint8_t fg, uint8_t bg) {
    return (uint8_t)(((bg & 0x0F) << 4) | (fg & 0x0F));
}

/* macro form, for use in static initializers / constant expressions */
#define VGA_ATTR(fg, bg)  ((uint8_t)((((bg) & 0x0F) << 4) | ((fg) & 0x0F)))

/* extract foreground / background from an attribute byte */
static inline uint8_t vga_attr_fg(uint8_t attr) { return attr & 0x0F; }
static inline uint8_t vga_attr_bg(uint8_t attr) { return (attr >> 4) & 0x0F; }

/* canonical 24-bit RGB values of the 16 VGA colors (indexed by enum vga_color).
   Defined in color.c. Used by the future framebuffer path; harmless now. */
extern const uint32_t vga_palette_rgb[16];

/* ---- FUTURE COLOR DEPTHS (framebuffer) -----------------------------------
 * When the framebuffer is brought up, the 8-bit (palettized) and 32-bit
 * (true-color RGB) models go here, e.g.:
 *
 *   typedef uint32_t rgb_t;
 *   static inline rgb_t color_rgb(uint8_t r, uint8_t g, uint8_t b) {
 *       return ((rgb_t)r << 16) | ((rgb_t)g << 8) | b;
 *   }
 *   rgb_t  color_vga_to_rgb(enum vga_color c);   // map the 16 VGA colors -> RGB
 *   uint8_t color_rgb_to_pal8(rgb_t c);          // nearest 8-bit palette index
 *
 * Deferred until a framebuffer consumer exists to test them. Keeping them in
 * this same file means callers include one "color.h" regardless of depth.
 * -------------------------------------------------------------------------- */

#endif