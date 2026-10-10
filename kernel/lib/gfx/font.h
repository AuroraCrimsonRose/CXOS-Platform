// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/gfx/font.h */
/* Aurora Tejeda */
/*
 * These are declarations, not font software, so this header keeps the kernel's
 * licence. The DATA they refer to does not: font.c is derived from Terminus Font
 * and is OFL-1.1. See that file and THIRD-PARTY-NOTICES.md.
 */

#ifndef FONT_DEFAULT_H
#define FONT_DEFAULT_H

#include <stdint.h>
#include "xfnt.h"

#define FONT_FIRST_CHAR  0x20   /* first glyph is space */
#define FONT_LAST_CHAR   0x7E   /* last glyph is '~'    */
#define FONT_GLYPH_COUNT 95

extern const uint8_t font_default_8x16[95][16];

/* ---- the active cell ------------------------------------------------------
 *
 * The cell is whatever the active font says, not a constant, so a loaded font
 * of another size changes the console's grid rather than being refused for not
 * being 8x16. Everything that lays out text asks these - fb_draw_char's loops,
 * fb_draw_string's advance, the console's cols/rows, the scroll step, and the
 * panic renderer - which is what "variable font size" amounts to: there is no
 * longer a second place that believes it knows the answer.
 *
 * Both return a non-zero value even if the font layer is somehow empty,
 * because every caller divides by them. A cell of zero would turn a panic into
 * a divide by zero, which is the one failure that must not happen on the path
 * that reports failures.
 */
static inline uint32_t font_cell_width(void) {
    uint32_t w = xfnt_active() ? xfnt_width() : 8u;
    return w ? w : 8u;
}

static inline uint32_t font_cell_height(void) {
    uint32_t h = xfnt_active() ? xfnt_height() : 16u;
    return h ? h : 16u;
}

/* Rows of the glyph for byte `ch`: font_cell_height() bytes, one per row,
 * MSB first.
 *
 * Indexing is by SLOT, and ASCII sits at its own value, so a loaded font prints
 * every byte this printed before. Taking an unsigned char is the whole reason
 * the extras are reachable: `char` is signed here, so a byte above 127 arrives
 * negative and the default font's range test rejects it. A loaded font covers
 * 0..255, and the high half is where its box drawing and blocks live.
 *
 * When a font IS loaded this never falls back to the compiled-in default, even
 * for a slot the font does not reach - it returns that font's slot 0 instead.
 * Mixing them would hand back 16 rows while font_cell_height() said 8, and the
 * caller would read past the glyph.
 */
static inline const uint8_t *font_glyph(unsigned char ch) {
    if (xfnt_active()) {
        const uint8_t *g = xfnt_glyph(ch);
        if (g) return g;
        /* A slot past what this font holds: blank, not the wrong glyph. Slot 0
           always exists, because a count of zero is refused at install. */
        return xfnt_glyph(0);
    }
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR) ch = FONT_FIRST_CHAR;
    return font_default_8x16[ch - FONT_FIRST_CHAR];
}

/* There was an 8x8 font here. It was removed: nothing ever called it, and its
   source font could not be identified, so it was the one asset in the tree whose
   provenance could not be demonstrated - and also the one nothing would miss. */

#endif