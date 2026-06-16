/* /CXLite/kernel/data/fonts/default.h */
/* Aurora Tejeda */

#ifndef FONT_DEFAULT_H
#define FONT_DEFAULT_H

#include <stdint.h>

#define FONT_FIRST_CHAR  0x20   /* first glyph is space */
#define FONT_LAST_CHAR   0x7E   /* last glyph is '~'    */
#define FONT_GLYPH_COUNT 95

extern const uint8_t font_default_8x16[95][16];
extern const uint8_t font_default_8x8[95][8];

/* Return pointer to the 16 bytes of the 8x16 glyph for character c.
   Out-of-range characters fall back to space. */
static inline const uint8_t *font_glyph_8x16(char c) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) c = FONT_FIRST_CHAR;
    return font_default_8x16[(unsigned char)c - FONT_FIRST_CHAR];
}

/* Return pointer to the 8 bytes of the 8x8 glyph for character c. */
static inline const uint8_t *font_glyph_8x8(char c) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) c = FONT_FIRST_CHAR;
    return font_default_8x8[(unsigned char)c - FONT_FIRST_CHAR];
}

#endif