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

#define FONT_FIRST_CHAR  0x20   /* first glyph is space */
#define FONT_LAST_CHAR   0x7E   /* last glyph is '~'    */
#define FONT_GLYPH_COUNT 95

extern const uint8_t font_default_8x16[95][16];

/* Return pointer to the 16 bytes of the 8x16 glyph for character c.
   Out-of-range characters fall back to space. */
static inline const uint8_t *font_glyph_8x16(char c) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) c = FONT_FIRST_CHAR;
    return font_default_8x16[(unsigned char)c - FONT_FIRST_CHAR];
}

/* There was an 8x8 font here. It was removed: nothing ever called it, and its
   source font could not be identified, so it was the one asset in the tree whose
   provenance could not be demonstrated - and also the one nothing would miss. */

#endif