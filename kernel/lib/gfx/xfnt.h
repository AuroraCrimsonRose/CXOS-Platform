// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/gfx/xfnt.h */
/* Aurora Tejeda / CATX Systems */
/*
 * XFNT - X Format foNT. A loadable bitmap font.
 *
 * The format is the bits and a header, nothing else. That is the point: the
 * fonts are drawn with a font editor which exports an AngelCode BMFont
 * descriptor plus a PNG atlas, and NEITHER of those belongs in a kernel. The
 * descriptor carries no pixels at all - they are in the PNG - so loading one
 * here would mean a PNG decoder (zlib inflate, chunk walking, CRCs, scanline
 * filters) and a text key=value parser, both in ring 0, both over a file that
 * arrived from a disk. `cxk font build` does that conversion on the host and
 * this reads a fixed layout, which is the same split CXEX already uses.
 *
 * Glyphs are indexed by SLOT, not by codepoint, so a lookup is a shift and an
 * add with no search and the console keeps its one-byte char. ASCII 32..126 sit
 * at their own values, so a loaded font prints every byte the console already
 * printed; the extras live from slot 128 up, and the optional codepoint table
 * records what each one means. The FONT therefore defines the codepage, rather
 * than a table in here claiming to.
 *
 * On-disk layout, little-endian:
 *
 *   0   u32  magic 'X','F','N','T'
 *   4   u16  version (XFNT_VERSION)
 *   6   u8   kind: 1 = bitmap, 2 = vector (reserved, refused here)
 *   7   u8   bytes per row; must equal (width + 7) / 8
 *   8   u8   glyph width in pixels
 *   9   u8   glyph height in pixels
 *   10  u16  glyph count
 *   12  u16  flags
 *   14  u16  reserved
 *   16  u8   face[8], NUL padded, NOT NUL terminated by contract
 *   24  ...  glyph_count * height * bytes_per_row bytes of rows
 *   ...  u32 codepoint[glyph_count], when XFNT_FLAG_CODEPOINTS is set
 *
 * `kind` is there because CX_DEVKIT_DESIGN.md section 5.1 locks XFNT as the
 * container for scalable vector fonts as well as bitmap ones. Only bitmap is
 * implemented; a vector font is refused by name rather than misread, and the
 * byte costs nothing now where a version bump later would not be free.
 *
 * Rows are MSB first - 0x80 is the leftmost pixel - which is the order
 * fb_draw_char already reads with (0x80 >> col), so nothing in the blit changes.
 */

#ifndef XFNT_H
#define XFNT_H

#include <stdint.h>

#define XFNT_VERSION 1

#define XFNT_MAGIC0 'X'
#define XFNT_MAGIC1 'F'
#define XFNT_MAGIC2 'N'
#define XFNT_MAGIC3 'T'

#define XFNT_HEADER_SIZE 24

#define XFNT_KIND_BITMAP 1
#define XFNT_KIND_VECTOR 2   /* reserved: the locked design's other half */

#define XFNT_FLAG_MSB_FIRST  0x0001u
#define XFNT_FLAG_CODEPOINTS 0x0002u

/* The ceilings, which are the format's own and not invented here - see
   XfntFormat.cs, which enforces the same ones so the converter cannot write a
   font this refuses. Width is capped at one byte per row for v1. */
#define XFNT_MAX_WIDTH   8
#define XFNT_MAX_HEIGHT  32
#define XFNT_MAX_GLYPHS  256

/* One resident font. 256 slots x 32 rows x 1 byte is the worst case the
   ceilings above permit, so the buffer is sized from them rather than from the
   font that happens to be loaded - a later, taller font needs no new thinking
   about whether it fits. */
#define XFNT_MAX_BYTES (XFNT_MAX_GLYPHS * XFNT_MAX_HEIGHT)

/* Validate `file` and install it as the resident font. Returns 0 on success, or
   a negative XFNT_E_* code; the previously installed font is left alone on any
   failure, so a bad font costs nothing. */
int xfnt_install(const uint8_t *file, uint32_t len);

#define XFNT_E_OK        0
#define XFNT_E_SHORT    -1   /* smaller than the header */
#define XFNT_E_MAGIC    -2   /* not an XFNT */
#define XFNT_E_VERSION  -3   /* a version this kernel does not implement */
#define XFNT_E_GEOMETRY -4   /* width/height/count/bytes_per_row out of range or inconsistent */
#define XFNT_E_LENGTH   -5   /* the header does not describe a file of this exact length */
#define XFNT_E_FLAGS    -6   /* a required flag is absent, or an unknown one is set */
#define XFNT_E_KIND     -7   /* a vector font, or a kind this version does not know */

/* 1 when a font is installed. */
int xfnt_active(void);

/* The installed font's cell, or 0 when none is installed. */
uint32_t xfnt_width(void);
uint32_t xfnt_height(void);
uint32_t xfnt_glyph_count(void);

/* Bytes per glyph for the installed font (height * bytes_per_row), or 0. */
uint32_t xfnt_glyph_bytes(void);

/* The rows of `slot`, or NULL when no font is installed or the slot is past
   what it holds. Never returns a short buffer: the pointer is good for
   xfnt_glyph_bytes() bytes. */
const uint8_t *xfnt_glyph(uint32_t slot);

/* The codepoint `slot` stands for, 0 if unknown or the font carries no table. */
uint32_t xfnt_codepoint(uint32_t slot);

/* Forget the installed font and go back to the compiled-in default. */
void xfnt_clear(void);

#endif
