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

/* Return pointer to the 16 bytes of the 8x16 glyph for character c.
 *
 * A loaded XFNT wins when one is installed AND its cell is exactly 8x16, which
 * is the geometry every caller of this function is built around - FB_CHAR_W and
 * FB_CHAR_H, the console's grid, the scroll arithmetic. A font of another size
 * is not refused at load time, because it is still useful to inspect and to
 * draw with directly; it simply does not become the console font, and the
 * compiled-in default keeps that job.
 *
 * Indexing is by SLOT, and ASCII sits at its own value, so a loaded font prints
 * every byte this printed before. The cast to unsigned is the whole reason the
 * extras are reachable at all: `char` is signed here, so a byte above 127
 * arrives negative and the default font's range test rejects it. A loaded font
 * covers 0..255, and the high half is where its box drawing and blocks live.
 */
static inline const uint8_t *font_glyph_8x16(char c) {
    unsigned char ch = (unsigned char)c;
    if (xfnt_active() && xfnt_width() == 8 && xfnt_height() == 16) {
        const uint8_t *g = xfnt_glyph(ch);
        if (g) return g;
        /* A slot the font does not reach: blank rather than the wrong glyph. */
        g = xfnt_glyph(0);
        if (g) return g;
    }
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) c = FONT_FIRST_CHAR;
    return font_default_8x16[(unsigned char)c - FONT_FIRST_CHAR];
}

/* There was an 8x8 font here. It was removed: nothing ever called it, and its
   source font could not be identified, so it was the one asset in the tree whose
   provenance could not be demonstrated - and also the one nothing would miss. */

#endif