/* /kernel/lib/color.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Color model implementation. The 16-color VGA model in color.h is entirely
 * inline (constants + bit packing), so this file is intentionally light right
 * now. It exists as the home for the non-inline color routines that arrive with
 * the framebuffer - the VGA-color -> 24/32-bit RGB table and the nearest-palette
 * search for 8-bit modes - so callers never have to learn a new file later.
 *
 * (When the framebuffer lands: add the rgb_t helpers and the vga->rgb table
 * here, declared in color.h at the marked seam.)
 */

#include "color.h"

/* The canonical RGB values of the 16 VGA text colors. Defined now (it's pure
   data, useful for reference and for the future framebuffer path) even though
   the text console doesn't consume it yet. Order matches enum vga_color. */
const uint32_t vga_palette_rgb[16] = {
    0x000000, /* black */        0x0000AA, /* blue */
    0x00AA00, /* green */        0x00AAAA, /* cyan */
    0xAA0000, /* red */          0xAA00AA, /* magenta */
    0xAA5500, /* brown */        0xAAAAAA, /* light grey */
    0x555555, /* dark grey */    0x5555FF, /* light blue */
    0x55FF55, /* light green */  0x55FFFF, /* light cyan */
    0xFF5555, /* light red */    0xFF55FF, /* light magenta */
    0xFFFF55, /* yellow */       0xFFFFFF  /* white */
};