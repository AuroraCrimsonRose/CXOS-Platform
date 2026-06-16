/* /CXLite/kernel/drivers/fb.c */
/* Aurora Tejeda */
/* Linear framebuffer driver - 32bpp and 16bpp direct-color VBE modes. */

#include "fb.h"
#include "default.h"

/* fixed addresses where vbe.asm stashed the mode info (must match vbe.asm) */
#define VBE_VALID   0x1C40
#define VBE_WIDTH   0x1C42
#define VBE_HEIGHT  0x1C44
#define VBE_BPP     0x1C46
#define VBE_PITCH   0x1C48
#define VBE_FB      0x1C4A

static int       active = 0;
static uint8_t  *fb = 0;        /* framebuffer base (linear) */
static uint32_t  width = 0;
static uint32_t  height = 0;
static uint32_t  bpp = 0;       /* 16 or 32 */
static uint32_t  pitch = 0;     /* bytes per scanline */
static enum fb_font cur_font = FB_FONT_8X16;  /* active text font */

int fb_init(void) {
    uint8_t valid = *(volatile uint8_t *)VBE_VALID;
    if (!valid) { active = 0; return -1; }

    width  = *(volatile uint16_t *)VBE_WIDTH;
    height = *(volatile uint16_t *)VBE_HEIGHT;
    bpp    = *(volatile uint8_t  *)VBE_BPP;
    pitch  = *(volatile uint16_t *)VBE_PITCH;
    fb     = (uint8_t *)(*(volatile uint32_t *)VBE_FB);

    /* we only handle 16 and 32 bpp here; anything else -> fall back */
    if ((bpp != 16 && bpp != 32) || fb == 0 || width == 0 || height == 0) {
        active = 0;
        return -1;
    }

    /* NOTE: the framebuffer's physical address is often high (e.g. 0xFD000000)
       and outside the kernel's low identity map. We do NOT map it here -
       paging_init() maps it (via fb_get_region) into the page tables it builds,
       so the mapping survives once paging is enabled. (Mapping it here would be
       wiped out when paging_init loads its own CR3.) */

    /* Auto-pick a default font by resolution: the 8x8 font is hard to read on
       a high-res screen (each glyph is physically tiny), so use the taller
       8x16 at >=1024 wide, and the compact 8x8 at lower resolutions where it
       stays legible and fits more text. (Still overridable via fb_set_font.) */
    cur_font = (width >= 1024) ? FB_FONT_8X16 : FB_FONT_8X8;

    active = 1;
    return 0;
}

int fb_active(void)      { return active; }

int fb_get_region(uint32_t *phys, uint32_t *size) {
    if (!active) { if (phys) *phys = 0; if (size) *size = 0; return 0; }
    if (phys) *phys = (uint32_t)fb;
    if (size) *size = pitch * height;
    return 1;
}
uint32_t fb_width(void)  { return width; }
uint32_t fb_height(void) { return height; }
uint32_t fb_bpp(void)    { return bpp; }

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (bpp == 32) {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    } else { /* 16bpp 5-6-5 */
        uint32_t rr = (r >> 3) & 0x1F;
        uint32_t gg = (g >> 2) & 0x3F;
        uint32_t bb = (b >> 3) & 0x1F;
        return (rr << 11) | (gg << 5) | bb;
    }
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!active || x >= width || y >= height) return;
    uint8_t *p = fb + y * pitch + x * (bpp / 8);
    if (bpp == 32) {
        *(uint32_t *)p = color;
    } else { /* 16 */
        *(uint16_t *)p = (uint16_t)color;
    }
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!active) return;
    uint32_t x1 = x + w, y1 = y + h;
    if (x1 > width)  x1 = width;
    if (y1 > height) y1 = height;
    for (uint32_t yy = y; yy < y1; yy++) {
        uint8_t *row = fb + yy * pitch + x * (bpp / 8);
        if (bpp == 32) {
            uint32_t *p = (uint32_t *)row;
            for (uint32_t xx = x; xx < x1; xx++) *p++ = color;
        } else {
            uint16_t *p = (uint16_t *)row;
            for (uint32_t xx = x; xx < x1; xx++) *p++ = (uint16_t)color;
        }
    }
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, width, height, color);
}

/* Bresenham line between (x0,y0) and (x1,y1). Integer-only, clips via
   fb_put_pixel's bounds check. */
void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    if (!active) return;
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0) fb_put_pixel((uint32_t)x0, (uint32_t)y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* scroll the whole framebuffer up by `pixels` rows (fast bulk copy), filling
   the newly-exposed bottom strip with `bg`. Far cheaper than re-rendering every
   glyph: a console scroll becomes one big copy + one new line of text. */
void fb_scroll_up(uint32_t pixels, uint32_t bg) {
    if (!active || pixels == 0 || pixels >= height) { fb_clear(bg); return; }

    uint32_t moved_rows = height - pixels;

    /* The surviving region is contiguous (whole-width scanlines back-to-back),
       so copy it as ONE block rather than per-scanline. Copy in 32-bit dwords
       (4 bytes/op) instead of byte-by-byte - far fewer memory operations, which
       matters a lot since the framebuffer is slow (often uncached MMIO).
       This is what makes scrolling fast. */
    uint8_t *dst = fb;
    uint8_t *src = fb + (uint32_t)pixels * pitch;
    uint32_t total = moved_rows * pitch;          /* bytes to move */

    uint32_t dwords = total >> 2;                 /* total / 4 */
    uint32_t tail   = total & 3;                  /* leftover bytes */

    uint32_t *d32 = (uint32_t *)dst;
    uint32_t *s32 = (uint32_t *)src;
    for (uint32_t i = 0; i < dwords; i++) d32[i] = s32[i];

    /* copy any trailing bytes (pitch is normally a multiple of 4, so usually none) */
    if (tail) {
        uint8_t *db = dst + (dwords << 2);
        uint8_t *sb = src + (dwords << 2);
        for (uint32_t i = 0; i < tail; i++) db[i] = sb[i];
    }

    /* clear the exposed bottom strip */
    fb_fill_rect(0, moved_rows, width, pixels, bg);
}

/* active font (default 8x16; fb_init may change it based on resolution) */
void fb_set_font(enum fb_font f) { cur_font = f; }
uint32_t fb_font_height(void) { return (cur_font == FB_FONT_8X8) ? 8 : 16; }

/* draw one glyph using the active font. each font byte is a row;
   bit 0x80 = leftmost pixel. */
void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
    if (!active) return;
    const uint8_t *glyph;
    uint32_t h;
    if (cur_font == FB_FONT_8X8) { glyph = font_glyph_8x8(c);  h = 8;  }
    else                         { glyph = font_glyph_8x16(c); h = 16; }

    for (uint32_t row = 0; row < h; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FB_CHAR_W; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

void fb_draw_string(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg) {
    if (!active) return;
    while (*s) {
        fb_draw_char(x, y, *s, fg, bg);
        x += FB_CHAR_W;
        s++;
    }
}