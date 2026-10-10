// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/video/fb.c */
/* Aurora Tejeda / CATX Systems */
/* Linear framebuffer driver - 16bpp (5-6-5) and 32bpp direct-color VBE modes. */

#include "fb.h"
#include "font.h"
#include "paging.h"

/* The kernel runs at 0xC0000000+; the boot page tables map the first 4 MB of
   physical RAM at 0xC0000000..0xC03FFFFF. The bootloader wrote the VBE info
   struct at PHYSICAL 0x1C40, so we reach it at 0x1C40 + KERNEL_VBASE. (The
   low identity map is dropped after boot, so the raw physical address is
   unmapped - this matches how pmm.c reaches the E820 map at 0x500.) */
#define KERNEL_VBASE    0xC0000000u
#define PHYS_TO_VIRT(p) ((volatile void *)((uint32_t)(p) + KERNEL_VBASE))

/* fixed PHYSICAL addresses where vbe.asm stashed the mode info */
#define VBE_VALID_PHYS   0x1C40
#define VBE_WIDTH_PHYS   0x1C42
#define VBE_HEIGHT_PHYS  0x1C44
#define VBE_BPP_PHYS     0x1C46
#define VBE_PITCH_PHYS   0x1C48
#define VBE_FB_PHYS      0x1C4A

/* Kernel-virtual window the LFB is mapped into. The high kernel address space
   is already carved up: heap at 0xD0000000 (grows up), the AHCI DMA window at
   0xE0000000 (ahci.c), and the recursive page-directory area at 0xFFC00000.
   So the framebuffer lives at 0xF0000000 - clear of all of them, with ~252 MB
   (0xF0000000..0xFFC00000) for any mode's LFB.
   IMPORTANT: must NOT overlap AHCI_DMA_VIRT. An earlier revision put both at
   0xE0000000; ahci_init() then remapped the top of the LFB onto DMA pages and
   corrupted the upper scanlines once storage came online (text drawn there lost
   its top rows). */
#define FB_VIRT_BASE     0xF0000000u

static int       active = 0;
static uint8_t  *fb = 0;        /* framebuffer base (VIRTUAL, mapped) */
static uint32_t  fb_phys = 0;   /* physical base (informational) */
static uint32_t  width = 0;
static uint32_t  height = 0;
static uint32_t  bpp = 0;       /* 16 or 32 */
static uint32_t  pitch = 0;     /* bytes per scanline */

int fb_init(void) {
    uint8_t valid = *(volatile uint8_t *)PHYS_TO_VIRT(VBE_VALID_PHYS);
    if (!valid) { active = 0; return -1; }

    width   = *(volatile uint16_t *)PHYS_TO_VIRT(VBE_WIDTH_PHYS);
    height  = *(volatile uint16_t *)PHYS_TO_VIRT(VBE_HEIGHT_PHYS);
    bpp     = *(volatile uint8_t  *)PHYS_TO_VIRT(VBE_BPP_PHYS);
    pitch   = *(volatile uint16_t *)PHYS_TO_VIRT(VBE_PITCH_PHYS);
    fb_phys = *(volatile uint32_t *)PHYS_TO_VIRT(VBE_FB_PHYS);

    /* we only handle 16 and 32 bpp; anything else -> fall back to text mode */
    if ((bpp != 16 && bpp != 32) || fb_phys == 0 || width == 0 || height == 0) {
        active = 0;
        return -1;
    }

    /* Map the LFB into our kernel-virtual window. The framebuffer's physical
       base is often high (e.g. 0xFD000000) and outside the low 4 MB the boot
       tables cover, so it must be mapped explicitly. The driver owns this
       mapping (like heap.c owns its window) rather than leaning on paging_init,
       which in v5 only adopts the boot PD + installs recursion.
       Mapped supervisor + writable; plain WB caching for now (write-combining
       via PAT/MTRR is a later performance pass). */
    uint32_t map_phys = fb_phys & ~0xFFFu;            /* page-align (LFB BARs already are) */
    uint32_t off      = fb_phys - map_phys;           /* normally 0 */
    uint32_t bytes    = pitch * height + off;
    uint32_t pages    = (bytes + 0xFFFu) >> 12;
    for (uint32_t i = 0; i < pages; i++) {
        paging_map_kernel(FB_VIRT_BASE + (i << 12),
                   map_phys     + (i << 12),
                   PAGE_WRITE);
    }
    fb = (uint8_t *)(FB_VIRT_BASE + off);

    active = 1;
    return 0;
}

int fb_active(void) { return active; }

int fb_get_region(uint32_t *phys, uint32_t *size) {
    if (!active) { if (phys) *phys = 0; if (size) *size = 0; return 0; }
    if (phys) *phys = fb_phys;
    if (size) *size = pitch * height;
    return 1;
}
uint32_t fb_width(void)  { return width; }
uint32_t fb_height(void) { return height; }
uint32_t fb_bpp(void)    { return bpp; }
uint32_t fb_pitch(void)  { return pitch; }

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
    if (bpp == 32) *(uint32_t *)p = color;
    else           *(uint16_t *)p = (uint16_t)color;
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

void fb_clear(uint32_t color) { fb_fill_rect(0, 0, width, height, color); }

/* Bresenham line; clips via fb_put_pixel's bounds check. */
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

/* Scroll the whole framebuffer up by `pixels` rows (fast bulk copy in dwords),
   filling the newly-exposed bottom strip with `bg`. The surviving region is
   contiguous, so it copies as one block - far cheaper than re-rendering, which
   matters since framebuffer memory is slow. */
void fb_scroll_up(uint32_t pixels, uint32_t bg) {
    if (!active || pixels == 0 || pixels >= height) { fb_clear(bg); return; }

    uint32_t moved_rows = height - pixels;
    uint8_t *dst = fb;
    uint8_t *src = fb + (uint32_t)pixels * pitch;
    uint32_t total  = moved_rows * pitch;     /* bytes to move */
    uint32_t dwords = total >> 2;
    uint32_t tail   = total & 3;

    uint32_t *d32 = (uint32_t *)dst;
    uint32_t *s32 = (uint32_t *)src;
    for (uint32_t i = 0; i < dwords; i++) d32[i] = s32[i];
    if (tail) {
        uint8_t *db = dst + (dwords << 2);
        uint8_t *sb = src + (dwords << 2);
        for (uint32_t i = 0; i < tail; i++) db[i] = sb[i];
    }
    fb_fill_rect(0, moved_rows, width, pixels, bg);
}

/* Scroll only a sub-rectangle up by `dy` pixels, filling the exposed bottom
   `dy` rows with `bg`. Unlike fb_scroll_up the surviving region isn't contiguous
   in memory (each text row is a slice of a wider scanline), so we copy row by
   row. Used by the framebuffer console to scroll its viewport without disturbing
   anything beside it (e.g. the boot logo in the other half of the screen). */
void fb_scroll_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint32_t dy, uint32_t bg) {
    if (!active || w == 0 || h == 0 || dy == 0) return;
    if (dy >= h) { fb_fill_rect(x, y, w, h, bg); return; }

    uint32_t bb        = bpp / 8;
    uint32_t row_bytes = w * bb;
    uint32_t move_rows = h - dy;
    for (uint32_t r = 0; r < move_rows; r++) {
        uint8_t *dst = fb + (y + r)      * pitch + x * bb;
        uint8_t *src = fb + (y + r + dy) * pitch + x * bb;   /* lower row, no overlap */
        uint32_t dwords = row_bytes >> 2;
        uint32_t tail   = row_bytes & 3;
        uint32_t *d = (uint32_t *)dst, *s = (uint32_t *)src;
        for (uint32_t i = 0; i < dwords; i++) d[i] = s[i];
        if (tail) {
            uint8_t *db = dst + (dwords << 2), *sb = src + (dwords << 2);
            for (uint32_t i = 0; i < tail; i++) db[i] = sb[i];
        }
    }
    fb_fill_rect(x, y + move_rows, w, dy, bg);
}

uint32_t fb_font_width(void)  { return font_cell_width(); }
uint32_t fb_font_height(void) { return font_cell_height(); }

/* Draw one glyph of the active font. Each font byte is a row of up to 8 pixels;
   bit 0x80 is the leftmost. A font narrower than 8 uses the high bits, which is
   the order `cxk font build` packs them in, so a 6-wide cell needs no special
   case here. */
void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
    if (!active) return;
    const uint8_t *glyph = font_glyph((unsigned char)c);
    if (!glyph) return;
    uint32_t cw = font_cell_width();
    uint32_t chh = font_cell_height();
    for (uint32_t row = 0; row < chh; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < cw; col++) {
            uint32_t color = (bits & (0x80u >> col)) ? fg : bg;
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

void fb_draw_string(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg) {
    if (!active) return;
    uint32_t cw = font_cell_width();
    while (*s) {
        fb_draw_char(x, y, *s, fg, bg);
        x += cw;
        s++;
    }
}

/* Length-bounded form, for strings whose terminator we do not trust: stops at
   the NUL or at `n` characters, whichever comes first. Use this for anything
   that came from ring 3 - see the note on FB_OP_DRAW_TEXT in sys_fb_op. */
void fb_draw_string_n(uint32_t x, uint32_t y, const char *s, uint32_t n,
                      uint32_t fg, uint32_t bg) {
    if (!active) return;
    uint32_t cw = font_cell_width();
    for (uint32_t i = 0; i < n && s[i]; i++) {
        fb_draw_char(x, y, s[i], fg, bg);
        x += cw;
    }
}


/* ---- SYS_FB_OP handler ----
 * Draw on behalf of a GRANT_FRAMEBUFFER holder. Colors cross the ABI as canonical
 * 0x00RRGGBB and are converted here via fb_rgb to the active mode. */
#include "../../cpu/usermode.h"   /* user_ptr_readable / user_ptr_writable */
#include "../../../abi/cxk_abi.h" /* fb_op_args, FB_OP_*, E_* */
#include "cxfs.h"                 /* FB_OP_SET_FONT reads the font itself */
#include "console.h"              /* the grid follows the font's cell */

/* Staging for a font being loaded, sized to the largest file XFNT permits:
   header + 256 glyphs of 32 rows + a codepoint per slot. Static rather than a
   local, for the reason cxfs.c keeps its block buffers out of frames - this
   runs on a syscall stack, and nine kilobytes is not something to put there. */
static uint8_t font_buf[XFNT_HEADER_SIZE + XFNT_MAX_GLYPHS * XFNT_MAX_HEIGHT
                        + XFNT_MAX_GLYPHS * 4];

static uint32_t fb_pack(uint32_t rgb) {
    return fb_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

int sys_fb_op(const struct fb_op_args *ua) {
    if (!user_ptr_readable((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct fb_op_args a = *ua;
    if (!fb_active()) return E_NOENT;

    switch (a.op) {
        case FB_OP_INFO: {
            if (!user_ptr_writable((uint32_t)a.out, 4 * sizeof(uint32_t))) return E_FAULT;
            a.out[0] = fb_width();
            a.out[1] = fb_height();
            a.out[2] = fb_bpp();
            a.out[3] = fb_pitch();
            return 0;
        }
        case FB_OP_CLEAR:     fb_clear(fb_pack(a.color)); return 0;
        case FB_OP_FILL_RECT: fb_fill_rect(a.x, a.y, a.w, a.h, fb_pack(a.color)); return 0;
        case FB_OP_PUT_PIXEL: fb_put_pixel(a.x, a.y, fb_pack(a.color)); return 0;
        case FB_OP_DRAW_LINE: fb_draw_line((int)a.x, (int)a.y, (int)a.w, (int)a.h, fb_pack(a.color)); return 0;
        case FB_OP_DRAW_TEXT: {
            /* Scan for a length we have actually validated, then draw AT MOST that
               many characters. Passing the bare pointer to fb_draw_string was a
               kernel-mode out-of-bounds read: the scan stops when user_ptr_readable fails
               at a page boundary, but fb_draw_string walks to its own NUL, so a
               string filling a mapped page with no terminator - or any string of
               0x1000 characters - ran straight off the end of the mapping. Since
               n != 0 in that case, the E_FAULT check below does not catch it.
               SYS_CONSOLE_WRITE has always done this correctly; this now matches it. */
            uint32_t n = 0;
            const char *p = a.text;
            while (n < 0x1000 && user_ptr_readable((uint32_t)a.text + n, 1) && p[n]) n++;
            if (n == 0 && !user_ptr_readable((uint32_t)a.text, 1)) return E_FAULT;
            fb_draw_string_n(a.x, a.y, a.text, n, fb_pack(a.color), fb_pack(a.color2));
            return (int)n;
        }
        case FB_OP_SET_FONT: {
            /* A NAME, not a path. The kernel composes /System/Fonts/<name>.xfnt
               and reads it itself, which is the SYS_EXEC_PATH shape: the bytes
               validated are the bytes installed, with no caller-held buffer in
               between, and GRANT_FRAMEBUFFER does not become a way to make the
               kernel read an arbitrary file. */
            char name[CXFS_NAME_LEN];
            uint32_t n = 0;
            for (;;) {
                if (n >= sizeof name) return E_INVAL;          /* no terminator in range */
                if (!user_ptr_readable((uint32_t)a.text + n, 1)) return E_FAULT;
                char c = a.text[n];
                if (c == '\0') break;
                /* Everything that could leave /System/Fonts is refused here, so
                   nothing downstream has to reason about traversal. '.' goes
                   too: the extension is ours to add, and without it ".." never
                   needs a special case. */
                if (c == '/' || c == '\\' || c == '.') return E_INVAL;
                if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) return E_INVAL;
                name[n++] = c;
            }
            if (n == 0) return E_INVAL;
            name[n] = '\0';

            static const char dir[] = "/System/Fonts/";
            static const char ext[] = ".xfnt";
            char path[FILE_PATH_MAX];
            /* (sizeof - 1) twice for the two NULs, then one back for the path's
               own. Checked rather than assumed, even though the three lengths
               are bounded well below FILE_PATH_MAX. */
            if ((sizeof dir - 1) + n + (sizeof ext - 1) + 1 > sizeof path) return E_INVAL;
            uint32_t p = 0;
            for (uint32_t i = 0; i < sizeof dir - 1; i++) path[p++] = dir[i];
            for (uint32_t i = 0; i < n; i++)              path[p++] = name[i];
            for (uint32_t i = 0; i < sizeof ext - 1; i++) path[p++] = ext[i];
            path[p] = '\0';

            struct cxfs_entry fe;
            if (cxfs_stat_path(path, &fe) != 0) return E_NOENT;
            if (fe.type != CXFS_TYPE_FILE)      return E_NOENT;
            if (fe.size == 0 || fe.size > sizeof font_buf) return E_INVAL;

            int got = cxfs_read_path(path, font_buf, (uint32_t)sizeof font_buf);
            if (got <= 0 || (uint64_t)got != fe.size) return E_NOENT;

            /* xfnt_install validates before it commits, so a font that is not
               one leaves the console drawing with whatever it had. */
            if (xfnt_install(font_buf, (uint32_t)got) != XFNT_E_OK) return E_INVAL;

            /* The cell may have changed, so the grid has to be recomputed - and
               here, unlike xfnt_install's callers in general, the screen is
               full of glyphs at the old size, so it is cleared too. */
            console_font_changed();
            console_clear();
            return 0;
        }
        default: return E_INVAL;
    }
}
