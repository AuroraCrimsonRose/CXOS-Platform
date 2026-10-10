// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/gfx/xfnt.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Loading a font off a disk is loading untrusted input, and the 2026-10-09
 * review is a list of what that costs when the fields are believed. So every
 * number in the header is checked before it is used to address anything, and
 * the checks are the SAME ones the converter makes (XfntFormat.Read), in the
 * same order - a host that could emit a font this refuses would move the
 * failure to boot, which is the shape of defect this file exists to avoid.
 *
 * The one that matters most is the length. The header describes a size; the
 * file has a size; they must be EXACTLY equal, computed in 64 bits. Nothing is
 * trusted to say how long it is - that is the CXFS superblock finding, and the
 * lesson generalises: a declared length is a claim, not a fact.
 */

#include "xfnt.h"
#include "string.h"

/* The resident font. .bss rather than the heap: it is loaded once at boot and
   the console must be able to draw before the heap is interesting. */
static uint8_t  fnt_bits[XFNT_MAX_BYTES];
static uint32_t fnt_codepoints[XFNT_MAX_GLYPHS];

static uint32_t fnt_width;
static uint32_t fnt_height;
static uint32_t fnt_count;
static uint32_t fnt_bpr;
static int      fnt_has_codepoints;
static int      fnt_loaded;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int xfnt_install(const uint8_t *file, uint32_t len) {
    if (!file) return XFNT_E_SHORT;
    if (len < XFNT_HEADER_SIZE) return XFNT_E_SHORT;

    if (file[0] != XFNT_MAGIC0 || file[1] != XFNT_MAGIC1 ||
        file[2] != XFNT_MAGIC2 || file[3] != XFNT_MAGIC3)
        return XFNT_E_MAGIC;

    if (rd16(file + 4) != XFNT_VERSION) return XFNT_E_VERSION;

    /* Bitmap only. A vector XFNT is a legitimate file this code cannot draw,
       so it is refused by kind rather than read as though its glyph data were
       rows of pixels. */
    if (file[6] != XFNT_KIND_BITMAP) return XFNT_E_KIND;

    uint32_t bpr    = file[7];
    uint32_t width  = file[8];
    uint32_t height = file[9];
    uint32_t count  = rd16(file + 10);
    uint16_t flags  = rd16(file + 12);

    if (width  < 1 || width  > XFNT_MAX_WIDTH)  return XFNT_E_GEOMETRY;
    if (height < 1 || height > XFNT_MAX_HEIGHT) return XFNT_E_GEOMETRY;
    if (count  < 1 || count  > XFNT_MAX_GLYPHS) return XFNT_E_GEOMETRY;
    /* bytes_per_row is not independent of width: it is derived from it, so a
       file is free to disagree and this is where that is caught. Believing it
       instead would let a 1-byte-per-row font claim 4 and walk the glyph data
       at four times the stride. */
    if (bpr != (width + 7) / 8) return XFNT_E_GEOMETRY;

    /* Rows must be MSB-first, because that is the only order the blit reads.
       Unknown flags are refused rather than ignored: a font written for a later
       version may mean something different by the same bytes, and accepting it
       silently is how a format stops meaning one thing. */
    if (!(flags & XFNT_FLAG_MSB_FIRST)) return XFNT_E_FLAGS;
    if (flags & ~(uint16_t)(XFNT_FLAG_MSB_FIRST | XFNT_FLAG_CODEPOINTS))
        return XFNT_E_FLAGS;

    int has_cp = (flags & XFNT_FLAG_CODEPOINTS) != 0;

    /* The exact-length test. 64-bit throughout, so no term can wrap past the
       comparison. */
    uint64_t need = (uint64_t)XFNT_HEADER_SIZE +
                    (uint64_t)count * height * bpr;
    if (has_cp) need += (uint64_t)count * 4u;
    if ((uint64_t)len != need) return XFNT_E_LENGTH;

    /* Belt and braces against the ceilings above having been changed without
       the buffer being resized. Cheap, local, and the kind of check whose
       absence is only discovered by overrunning .bss. */
    if ((uint64_t)count * height * bpr > (uint64_t)XFNT_MAX_BYTES)
        return XFNT_E_GEOMETRY;

    /* Everything is validated; commit. Nothing above this line has touched the
       installed font, so a refused font leaves the previous one working. */
    uint32_t bytes = count * height * bpr;
    memcpy(fnt_bits, file + XFNT_HEADER_SIZE, bytes);

    for (uint32_t i = 0; i < XFNT_MAX_GLYPHS; i++) fnt_codepoints[i] = 0;
    if (has_cp) {
        const uint8_t *cp = file + XFNT_HEADER_SIZE + bytes;
        for (uint32_t i = 0; i < count; i++)
            fnt_codepoints[i] = rd32(cp + i * 4u);
    }

    fnt_width  = width;
    fnt_height = height;
    fnt_count  = count;
    fnt_bpr    = bpr;
    fnt_has_codepoints = has_cp;
    fnt_loaded = 1;
    return XFNT_E_OK;
}

int xfnt_active(void) { return fnt_loaded; }

uint32_t xfnt_width(void)       { return fnt_loaded ? fnt_width  : 0; }
uint32_t xfnt_height(void)      { return fnt_loaded ? fnt_height : 0; }
uint32_t xfnt_glyph_count(void) { return fnt_loaded ? fnt_count  : 0; }
uint32_t xfnt_glyph_bytes(void) { return fnt_loaded ? fnt_height * fnt_bpr : 0; }

const uint8_t *xfnt_glyph(uint32_t slot) {
    if (!fnt_loaded || slot >= fnt_count) return 0;
    return fnt_bits + (uint32_t)slot * fnt_height * fnt_bpr;
}

uint32_t xfnt_codepoint(uint32_t slot) {
    if (!fnt_loaded || !fnt_has_codepoints || slot >= fnt_count) return 0;
    return fnt_codepoints[slot];
}

void xfnt_clear(void) {
    fnt_loaded = 0;
    fnt_width = fnt_height = fnt_count = fnt_bpr = 0;
    fnt_has_codepoints = 0;
}
