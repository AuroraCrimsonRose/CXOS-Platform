// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/video/fb.c */
/* Aurora Tejeda / CATX Systems */
/* Linear framebuffer driver - 16bpp (5-6-5) and 32bpp direct-color VBE modes. */

#include "fb.h"
#include "font.h"
#include "arch/paging.h"
#include "boot/cxbi.h"

/* Kernel-virtual window the LFB is mapped into, placed with the rest of the
   kernel half in arch/<arch>/paging_defs.h.
   IMPORTANT: must NOT overlap AHCI_DMA_VIRT. An earlier revision put both at
   0xE0000000; ahci_init() then remapped the top of the LFB onto DMA pages and
   corrupted the upper scanlines once storage came online (text drawn there lost
   its top rows). */
#define FB_VIRT_BASE     KVA_FRAMEBUFFER

static int       active = 0;
static uint8_t  *fb = 0;        /* framebuffer base (VIRTUAL, mapped) */
static uint32_t  fb_phys = 0;   /* physical base (informational) */
static uint32_t  width = 0;
static uint32_t  height = 0;
static uint32_t  bpp = 0;       /* 16 or 32 */
static uint32_t  pitch = 0;     /* bytes per scanline */

/* The pixel layouts fb_rgb packs: 5-6-5 at 16 bpp and 8-8-8 at 32, red
   highest. A mode in any other order (BGR, as UEFI GOP often gives) would
   draw every colour wrong, so it is refused rather than used. */
static int layout_ok(const struct cxk_boot_info *b) {
    if (b->fb_bpp == 32)
        return b->fb_red_bits == 8 && b->fb_red_shift == 16 &&
               b->fb_green_bits == 8 && b->fb_green_shift == 8 &&
               b->fb_blue_bits == 8 && b->fb_blue_shift == 0;
    if (b->fb_bpp == 16)
        return b->fb_red_bits == 5 && b->fb_red_shift == 11 &&
               b->fb_green_bits == 6 && b->fb_green_shift == 5 &&
               b->fb_blue_bits == 5 && b->fb_blue_shift == 0;
    return 0;
}

int fb_init(void) {
    /* the mode the loader set, from the handover (boot/cxbi.h) */
    const struct cxk_boot_info *b = cxbi();
    active = 0;
    if (b->fb_addr == 0) return -1;              /* text mode */
    if ((b->fb_addr >> 32) || (b->flags & CXBI_FLAG_FB_ABOVE_4G)) return -1;

    width   = b->fb_width;
    height  = b->fb_height;
    bpp     = b->fb_bpp;
    pitch   = b->fb_pitch;
    fb_phys = (uint32_t)b->fb_addr;

    /* we only handle 16 and 32 bpp in fb_rgb's layout; else text mode */
    if (!layout_ok(b) || width == 0 || height == 0 || pitch < width * (bpp / 8))
        return -1;

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
                   VM_WRITE);
    }
    fb = (uint8_t *)(uintptr_t)(FB_VIRT_BASE + off);

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

uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (!active || x >= width || y >= height) return 0;
    const uint8_t *p = fb + y * pitch + x * (bpp / 8);
    return bpp == 32 ? *(const uint32_t *)p : *(const uint16_t *)p;
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
   that came from ring 3 - see the note in sys_fb_draw_text. */
void fb_draw_string_n(uint32_t x, uint32_t y, const char *s, uint32_t n,
                      uint32_t fg, uint32_t bg) {
    if (!active) return;
    uint32_t cw = font_cell_width();
    for (uint32_t i = 0; i < n && s[i]; i++) {
        fb_draw_char(x, y, s[i], fg, bg);
        x += cw;
    }
}


/* ---- the SYS_FB_* handlers ----
 * Draw on behalf of a GRANT_FRAMEBUFFER holder. Colors cross the ABI as canonical
 * 0x00RRGGBB and are converted here via fb_rgb to the active mode. */
#include "../../cpu/usermode.h"   /* user_ptr_readable / user_ptr_writable */
#include "../../../abi/cxk_abi.h" /* fb_geometry, E_* */
#include "cxfs.h"                 /* SYS_FB_SET_FONT reads the font itself */
#include "console.h"              /* the grid follows the font's cell */

/* Staging for a font being loaded, sized to the largest file XFNT permits:
   header + 256 glyphs of 32 rows + a codepoint per slot. Static rather than a
   local, for the reason cxfs.c keeps its block buffers out of frames - this
   runs on a syscall stack, and nine kilobytes is not something to put there. */
static uint8_t font_buf[XFNT_HEADER_SIZE + XFNT_MAX_GLYPHS * XFNT_MAX_HEIGHT
                        + XFNT_MAX_GLYPHS * 4];

/* Where a font may come from, in the order it is searched.
 *
 * /System first is not a detail. Both directories hold signed-by-nobody data -
 * a font is not executable and carries no signature - so the difference is who
 * may WRITE them: /System is the OS, /Shared is group-writable. Searching the
 * user-writable one first would let anyone shadow a system font invisibly,
 * which is the reason CX_FILESYSTEM_LAYOUT.md already gives for
 * /System/Programs coming before /Shared/Programs. Same hazard, same order. */
static const char *const font_dirs[] = {
    "/System/Fonts/",
    "/Shared/Fonts/",
};

/* Find <name>.xfnt in the search path, read it, validate it, and install it.
 *
 * `name` is a bare name that has ALREADY been checked for path separators by
 * whoever accepted it from outside - sys_fb_set_font does that before calling here.
 * Does not clear the console: the grid is recomputed, but what to do about the
 * glyphs already drawn is the caller's decision.
 *
 * Returns 0, E_NOT_FOUND if no directory holds it, or E_INVALID_ARGS if what was found is
 * not a font this kernel can use. */
int fb_font_select(const char *name) {
    static const char ext[] = ".xfnt";
    if (!name || !name[0]) return E_INVALID_ARGS;

    /* The composed filename is <name>.xfnt, and that is the name CXFS has to
       store, so the extension counts against the name field. Measuring `name`
       alone against CXFS_NAME_LEN accepted names cxfs_normalize_name can never
       keep: they got as far as cxfs_stat_path and then always missed, so a
       too-long name answered "no such font" rather than "that is not a name". */
    uint32_t n = 0;
    while (name[n]) {
        /* After the increment, so `n` is a LENGTH when it is measured. Tested
           before it, as the original was, `n` is an index and the bound lands
           one character too late - which is how the first version of this
           rejected 60 and let 59 through, and is why both sides of the limit
           are asserted in ktest rather than only the refusal. */
        n++;
        if (n + (sizeof ext - 1) >= CXFS_NAME_LEN) return E_INVALID_ARGS;
    }

    for (unsigned d = 0; d < sizeof font_dirs / sizeof font_dirs[0]; d++) {
        const char *dir = font_dirs[d];
        uint32_t dlen = 0;
        while (dir[dlen]) dlen++;

        char path[FILE_PATH_MAX];
        /* Checked rather than assumed, even though the three lengths are
           bounded well below FILE_PATH_MAX. */
        if (dlen + n + (sizeof ext - 1) + 1 > sizeof path) return E_INVALID_ARGS;
        uint32_t p = 0;
        for (uint32_t i = 0; i < dlen; i++)           path[p++] = dir[i];
        for (uint32_t i = 0; i < n; i++)              path[p++] = name[i];
        for (uint32_t i = 0; i < sizeof ext - 1; i++) path[p++] = ext[i];
        path[p] = '\0';

        struct cxfs_entry fe;
        if (cxfs_stat_path(path, &fe) != 0)  continue;   /* not here; try the next */
        if (fe.type != CXFS_TYPE_FILE)       continue;
        if (fe.size == 0 || fe.size > sizeof font_buf) return E_INVALID_ARGS;

        int got = cxfs_read_path(path, font_buf, (uint32_t)sizeof font_buf);
        if (got <= 0 || (uint64_t)got != fe.size) return E_INVALID_ARGS;

        /* xfnt_install validates before it commits, so a file that is not a
           font leaves the console drawing with whatever it had. The search
           stops at the first directory holding the NAME - a broken font in
           /System is an error, not a reason to fall through to /Shared and
           silently use a different one. */
        if (xfnt_install(font_buf, (uint32_t)got) != XFNT_E_OK) return E_INVALID_ARGS;
        console_font_changed();
        return 0;
    }
    /* Falling out means no directory held the name at all. Every path that DID
       match returns from inside the loop - a font that is present but unusable
       is E_INVALID_ARGS there - so there is no "found but rejected" case left to carry
       out here, and a flag implying one only misleads the next reader. */
    return E_NOT_FOUND;
}

static uint32_t fb_pack(uint32_t rgb) {
    return fb_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

/* Each answers E_NOT_FOUND with no framebuffer, before it looks at a pointer:
   in VGA text mode there is nothing for any of them to do. */
int sys_fb_info(uint64_t ug) {
    if (!fb_active()) return E_NOT_FOUND;
    if (!user_ptr_writable(ug, sizeof(struct fb_geometry))) return E_BAD_ADDRESS;
    struct fb_geometry *g = (struct fb_geometry *)user_ptr(ug);
    g->width  = fb_width();
    g->height = fb_height();
    g->bpp    = fb_bpp();
    g->pitch  = fb_pitch();
    return 0;
}

int sys_fb_clear(uint32_t color) {
    if (!fb_active()) return E_NOT_FOUND;
    fb_clear(fb_pack(color));
    return 0;
}

int sys_fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_active()) return E_NOT_FOUND;
    fb_fill_rect(x, y, w, h, fb_pack(color));
    return 0;
}

int sys_fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_active()) return E_NOT_FOUND;
    fb_put_pixel(x, y, fb_pack(color));
    return 0;
}

int sys_fb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    if (!fb_active()) return E_NOT_FOUND;
    fb_draw_line((int)x0, (int)y0, (int)x1, (int)y1, fb_pack(color));
    return 0;
}

int sys_fb_draw_text(uint32_t x, uint32_t y, uint64_t text, uint32_t fg, uint32_t bg) {
    if (!fb_active()) return E_NOT_FOUND;
    /* Scan for a length we have actually validated, then draw AT MOST that
       many characters. Passing the bare pointer to fb_draw_string was a
       kernel-mode out-of-bounds read: the scan stops when user_ptr_readable fails
       at a page boundary, but fb_draw_string walks to its own NUL, so a
       string filling a mapped page with no terminator - or any string of
       0x1000 characters - ran straight off the end of the mapping. Since
       n != 0 in that case, the E_BAD_ADDRESS check below does not catch it.
       SYS_CONSOLE_WRITE has always done this correctly; this now matches it. */
    uint32_t n = 0;
    const char *p = user_ptr(text);
    while (n < 0x1000 && user_ptr_readable(text + n, 1) && p[n]) n++;
    if (n == 0 && !user_ptr_readable(text, 1)) return E_BAD_ADDRESS;
    fb_draw_string_n(x, y, p, n, fb_pack(fg), fb_pack(bg));
    return (int)n;
}

int sys_fb_set_font(uint64_t uname) {
    if (!fb_active()) return E_NOT_FOUND;
    /* A NAME, not a path. The kernel composes /System/Fonts/<name>.xfnt
       and reads it itself, which is the SYS_EXEC_PATH shape: the bytes
       validated are the bytes installed, with no caller-held buffer in
       between, and GRANT_FRAMEBUFFER does not become a way to make the
       kernel read an arbitrary file. */
    char name[CXFS_NAME_LEN];
    uint32_t n = 0;
    for (;;) {
        if (n >= sizeof name) return E_INVALID_ARGS;          /* no terminator in range */
        if (!user_ptr_readable(uname + n, 1)) return E_BAD_ADDRESS;
        char c = ((const char *)user_ptr(uname))[n];
        if (c == '\0') break;
        /* Everything that could leave /System/Fonts is refused here, so
           nothing downstream has to reason about traversal. '.' goes
           too: the extension is ours to add, and without it ".." never
           needs a special case. */
        if (c == '/' || c == '\\' || c == '.') return E_INVALID_ARGS;
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) return E_INVALID_ARGS;
        name[n++] = c;
    }
    if (n == 0) return E_INVALID_ARGS;
    name[n] = '\0';

    /* Only the syscall clears, and only when the GRID changed.
       fb_font_select is also what the self-tests use, and wiping the
       boot log out from under them would be collateral.

       A different cell makes what is on screen garbage: the pixels no
       longer line up with the rows the console believes it has, and the
       honest response is to start again. A font with the SAME cell
       leaves old text in old glyphs above the cursor, correctly placed
       and perfectly readable, just in a different face - clearing for
       that buys nothing and costs a boot log nobody can scroll back to.
       Not hypothetical: the shell restores a saved font at startup, so
       an unconditional clear meant every boot after the first `font`
       command erased the boot log the moment the shell came up. */
    uint32_t cw = font_cell_width();
    uint32_t ch = font_cell_height();
    int rc = fb_font_select(name);
    if (rc != 0) return rc;
    if (font_cell_width() != cw || font_cell_height() != ch) console_clear();
    return 0;
}
