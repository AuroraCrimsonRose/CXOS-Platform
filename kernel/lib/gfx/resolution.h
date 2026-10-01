/* /kernel/lib/gfx/resolution.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * Canonical catalog of display resolutions. PURE - constants, lookups, and
 * classification only, no hardware and no state - so it lives in lib/. This is
 * the single home for "what a resolution is" in CXK (the same role color.c
 * plays for color): the boot log names the active mode from here, the console
 * derives its 8x16 text grid from here, and the future GUI gates on capability
 * tier (>= SVGA) while a future runtime mode picker reads the catalog.
 *
 * This MIRRORS - it does not share - the boot-time preference table in
 * boot/vbe.asm. They can't share storage (different toolchains, different boot
 * phases): vbe.asm picks the mode in real mode before the kernel exists; this
 * catalog is the kernel's C-side knowledge of the mode it ended up in. Keep the
 * two in rough sync by hand when adding resolutions.
 *
 * Intentionally minimal: no aspect-ratio helpers yet (nothing consumes them).
 * They slot in when the GUI needs them, with a consumer to test against.
 */

#ifndef RESOLUTION_H
#define RESOLUTION_H

#include <stdint.h>

/* Coarse capability tier, ORDERED low->high so comparisons work. The GUI will
   require at least RES_TIER_SVGA (smaller screens can't fit the chrome). */
enum res_tier {
    RES_TIER_SUB_VGA = 0,   /* below 640x480 (CGA/EGA-era) */
    RES_TIER_VGA,           /* 640x480 */
    RES_TIER_SVGA,          /* 800x600 up to just under XGA */
    RES_TIER_XGA_PLUS       /* 1024x768 and larger */
};

/* One catalog entry. */
struct res_info {
    uint16_t      w;
    uint16_t      h;
    const char   *name;     /* "VGA", "SVGA", "XGA", ... */
    enum res_tier tier;
};

/* Exact (w,h) lookup. Returns NULL if the resolution isn't catalogued. */
const struct res_info *res_lookup(uint32_t w, uint32_t h);

/* Human name for (w,h); returns "custom" (never NULL) if not catalogued. */
const char *res_name(uint32_t w, uint32_t h);

/* Capability tier for ANY (w,h) - computed from the dimensions, so an
   off-catalog mode still classifies correctly. */
enum res_tier res_classify(uint32_t w, uint32_t h);

/* Is (w,h) at least SVGA, the GUI minimum? 1 = yes, 0 = no. */
int res_is_gui_capable(uint32_t w, uint32_t h);

/* Text-grid geometry for the 8x16 console font (CXK's only text font now;
   the 8x8 glyphs stay in the font lib but are no longer used for the console). */
static inline uint32_t res_text_cols(uint32_t w) { return w / 8u; }
static inline uint32_t res_text_rows(uint32_t h) { return h / 16u; }

#endif