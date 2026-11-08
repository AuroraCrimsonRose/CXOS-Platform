// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/video/fb.c */
/* Aurora Tejeda / CATX Systems */
/* Linear framebuffer driver - 16bpp (5-6-5) and 32bpp direct-color VBE modes. */

#include "fb.h"
#include "font.h"
#include "arch/paging.h"
#include "boot/cxbi.h"
#include "pmm.h"

/* Kernel-virtual window the LFB is mapped into, placed with the rest of the
   kernel half in arch/<arch>/paging_defs.h.
   IMPORTANT: must NOT overlap AHCI_DMA_VIRT. An earlier revision put both at
   0xE0000000; ahci_init() then remapped the top of the LFB onto DMA pages and
   corrupted the upper scanlines once storage came online (text drawn there lost
   its top rows). */
#define FB_VIRT_BASE     KVA_FRAMEBUFFER

/* ---- the copy in RAM -------------------------------------------------------
 * Every drawing call draws into `back`, a copy of the screen in RAM laid out
 * as the screen is (same pitch), then writes the rectangle it changed to the
 * screen. Nothing reads the screen: a scroll moves the copy's rows and
 * rewrites the screen from it, and fb_get_pixel reads the copy.
 *
 * Why: on the rig (FX-8350, W5000) the framebuffer sits in the PCI hole,
 * which the firmware's MTRRs mark uncached. Each read of it is a PCIe round
 * trip of about a microsecond, and fb_scroll_rect used to read the whole
 * console viewport back on every line printed: the boot took minutes to
 * reach the prompt. Writes are posted, and with the screen mapped VM_WC
 * (PAT entry 4, which wins over the MTRR) they go out combined. Under QEMU
 * the screen is host RAM and none of this shows.
 *
 * XNU keeps a copy too, of the console's cells rather than its pixels, and
 * repaints only the cells a scroll changed (osfmk/console/video_console.c,
 * gc_scroll_up); Serenity and Haiku move the rows in the framebuffer itself
 * (Kernel/Devices/GPU/Console/GenericFramebufferConsole.cpp, scroll_up;
 * src/system/kernel/debug/frame_buffer_console.cpp, console_blit). A pixel
 * copy covers every caller (the console, the logo, the panic screen, the
 * SYS_FB_* calls) at one place.
 *
 * When no copy can be had, `back` is the screen and the writes to it are
 * skipped: the old behaviour, slow on the rig but right. */
#define FB_COPY_BASE     KVA_FB_COPY

static int       active = 0;
static uint8_t  *fb = 0;        /* the screen (VIRTUAL, mapped) */
static uint8_t  *back = 0;      /* where drawing happens: the copy, or fb if none */
static uint32_t  fb_phys = 0;   /* physical base (informational) */
static uint32_t  width = 0;
static uint32_t  height = 0;
static uint32_t  bpp = 0;       /* 16 or 32 */
static uint32_t  pitch = 0;     /* bytes per scanline */

/* ---- the screen given away (2.3, SYS_SCREEN_OPEN) ---------------------------
 * While the display server holds the screen as a memory object, the kernel
 * draws only into its copy: `flush` writes nothing to the screen. Both flags
 * change under the log lock, which every drawing call is made under (the
 * console's, SYS_FB_*'s), so a flush is never half before and half after.
 *   held  - a KMEM_SCREEN object lives; cleared by its destroy, which puts the
 *           copy back on the screen.
 *   taken - a panic took the screen back (fb_panic_takeover); never cleared,
 *           so a holder still running elsewhere does not get it again.
 * Read without the lock on the panic path, which runs with the log broken. */
static volatile int held  = 0;
static volatile int taken = 0;

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

/* Copy n bytes forward, in the widest words both ends allow. The scrolls'
   rows never overlap, and a write to the screen should be as few and as
   wide as it can be. */
static void copy_fwd(uint8_t *d, const uint8_t *s, uint32_t n) {
    uint32_t i = 0;
    if ((((uintptr_t)d | (uintptr_t)s) & 7u) == 0) {
        for (; i + 8 <= n; i += 8) *(uint64_t *)(d + i) = *(const uint64_t *)(s + i);
    } else if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0) {
        for (; i + 4 <= n; i += 4) *(uint32_t *)(d + i) = *(const uint32_t *)(s + i);
    }
    for (; i < n; i++) d[i] = s[i];
}

/* Clip (x, y, w, h) to the screen; 0 if nothing is left. */
static int clip(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) {
    if (*x >= width || *y >= height || *w == 0 || *h == 0) return 0;
    if (*w > width - *x)  *w = width - *x;
    if (*h > height - *y) *h = height - *y;
    return 1;
}

/* Does the kernel's drawing go on to the screen? Not when the copy IS the
   screen (nothing more to write), nor while the display server holds it. */
static int to_screen(void) { return back != fb && !(held && !taken); }

/* Write the copy's rectangle to the screen, row by row. */
static void flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!to_screen() || !clip(&x, &y, &w, &h)) return;
    uint32_t bb = bpp / 8, off = y * pitch + x * bb;
    for (uint32_t r = 0; r < h; r++, off += pitch)
        copy_fwd(fb + off, back + off, w * bb);
}

/* The copy: pages of RAM at FB_COPY_BASE, one by one, so no contiguous run is
   needed. Cleared, and the screen with it, so the two agree from here on. On
   any failure the pages go back and drawing goes to the screen itself. */
static void copy_init(uint32_t bytes) {
    uint32_t pages = (bytes + 0xFFFu) >> 12;
    back = fb;
    for (uint32_t i = 0; i < pages; i++) {
        paddr_t p = pmm_alloc();
        if (p) paging_map_kernel(FB_COPY_BASE + ((uintptr_t)i << 12), p, VM_WRITE);
        if (!p || paging_get_phys(FB_COPY_BASE + ((uintptr_t)i << 12)) != p) {
            if (p) pmm_free(p);
            while (i--) {
                paddr_t old = paging_unmap(FB_COPY_BASE + ((uintptr_t)i << 12));   /* then freed */
                if (old) pmm_free(old);
            }
            return;
        }
    }
    back = (uint8_t *)(uintptr_t)FB_COPY_BASE;
    uint64_t *q = (uint64_t *)back;
    for (uint32_t i = 0; i < (pages << 12) / 8; i++) q[i] = 0;
    flush(0, 0, width, height);
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
       Mapped supervisor + writable, write-combining (VM_WC; arch/paging.h):
       nothing reads it, so the writes may be merged and reordered. */
    uint32_t map_phys = fb_phys & ~0xFFFu;            /* page-align (LFB BARs already are) */
    uint32_t off      = fb_phys - map_phys;           /* normally 0 */
    uint32_t bytes    = pitch * height + off;
    uint32_t pages    = (bytes + 0xFFFu) >> 12;
    for (uint32_t i = 0; i < pages; i++) {
        paging_map_kernel(FB_VIRT_BASE + (i << 12),
                   map_phys     + (i << 12),
                   VM_WRITE | VM_WC);
    }
    fb = (uint8_t *)(uintptr_t)(FB_VIRT_BASE + off);

    copy_init(pitch * height);
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

static void store(uint8_t *base, uint32_t x, uint32_t y, uint32_t color) {
    uint8_t *p = base + y * pitch + x * (bpp / 8);
    if (bpp == 32) *(uint32_t *)p = color;
    else           *(uint16_t *)p = (uint16_t)color;
}

static uint32_t load(const uint8_t *base, uint32_t x, uint32_t y) {
    const uint8_t *p = base + y * pitch + x * (bpp / 8);
    return bpp == 32 ? *(const uint32_t *)p : *(const uint16_t *)p;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!active || x >= width || y >= height) return;
    store(back, x, y, color);
    if (to_screen()) store(fb, x, y, color);
}

/* From the copy: the screen is never read (above). */
uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (!active || x >= width || y >= height) return 0;
    return load(back, x, y);
}

/* Fill the copy's rectangle, without writing the screen. */
static void fill_back(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t yy = y; yy < y + h; yy++) {
        uint8_t *row = back + yy * pitch + x * (bpp / 8);
        if (bpp == 32) {
            uint32_t *p = (uint32_t *)row;
            for (uint32_t i = 0; i < w; i++) *p++ = color;
        } else {
            uint16_t *p = (uint16_t *)row;
            for (uint32_t i = 0; i < w; i++) *p++ = (uint16_t)color;
        }
    }
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!active || !clip(&x, &y, &w, &h)) return;
    fill_back(x, y, w, h, color);
    flush(x, y, w, h);
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

/* Scroll only a sub-rectangle up by `dy` pixels, filling the exposed bottom
   `dy` rows with `bg`: the copy's rows move, a row at a time (each text row
   is a slice of a wider scanline), then the screen is written from it. Used by the framebuffer console to scroll its viewport without disturbing
   anything beside it (e.g. the boot logo in the other half of the screen). */
void fb_scroll_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint32_t dy, uint32_t bg) {
    if (!active || !clip(&x, &y, &w, &h) || dy == 0) return;
    if (dy >= h) { fb_fill_rect(x, y, w, h, bg); return; }

    uint32_t bb        = bpp / 8;
    uint32_t move_rows = h - dy;
    for (uint32_t r = 0; r < move_rows; r++)
        copy_fwd(back + (y + r) * pitch + x * bb,
                 back + (y + r + dy) * pitch + x * bb, w * bb);   /* lower row, no overlap */
    fill_back(x, y + move_rows, w, dy, bg);
    flush(x, y, w, h);
}

uint32_t fb_font_width(void)  { return font_cell_width(); }
uint32_t fb_font_height(void) { return font_cell_height(); }

/* Draw one glyph of the active font. Each font byte is a row of up to 8 pixels;
   bit 0x80 is the leftmost. A font narrower than 8 uses the high bits, which is
   the order `cxk font build` packs them in, so a 6-wide cell needs no special
   case here. Drawn into the copy, then the cell written to the screen a row
   at a time. */
void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
    if (!active) return;
    const uint8_t *glyph = font_glyph((unsigned char)c);
    if (!glyph) return;
    uint32_t cw = font_cell_width(), chh = font_cell_height();
    uint32_t w = cw, h = chh;
    if (!clip(&x, &y, &w, &h)) return;
    for (uint32_t row = 0; row < h; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < w; col++)
            store(back, x + col, y + row, (bits & (0x80u >> col)) ? fg : bg);
    }
    flush(x, y, w, h);
}

#if CXK_TEST_HOOKS
int fb_test_has_copy(void) { return active && back != fb; }

uint32_t fb_test_screen_pixel(uint32_t x, uint32_t y) {
    if (!active || x >= width || y >= height) return 0;
    return load(fb, x, y);
}

void fb_test_screen_poke(uint32_t x, uint32_t y, uint32_t color) {
    if (active && x < width && y < height) store(fb, x, y, color);
}

uintptr_t fb_test_screen_virt(void) { return (uintptr_t)fb; }

uint32_t fb_test_disagree(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t n = 0;
    if (!active || !clip(&x, &y, &w, &h)) return 0;
    for (uint32_t yy = y; yy < y + h; yy++)
        for (uint32_t xx = x; xx < x + w; xx++)
            if (load(fb, xx, yy) != load(back, xx, yy)) n++;
    return n;
}
#endif

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
#include "disk.h"                 /* the storage lock over a font's selection */

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
    /* font_buf is one buffer: the storage lock, first in the order and taken
       by the reads below anyway, keeps a second CPU's selection out of it
       (1.9e4). */
    STORAGE_LOCKED();

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
        /* Installed under the log lock: the console reads the glyphs to draw
           every line, on any CPU. */
        LOG_LOCKED();
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
    struct fb_geometry g = { 0 };
    g.width  = fb_width();
    g.height = fb_height();
    g.bpp    = fb_bpp();
    g.pitch  = fb_pitch();
    return user_copy_out(ug, &g, sizeof g) == 0 ? 0 : E_BAD_ADDRESS;
}

int sys_fb_clear(uint32_t color) {
    if (!fb_active()) return E_NOT_FOUND;
    LOG_LOCKED();   /* the console draws here too, under it */
    fb_clear(fb_pack(color));
    return 0;
}

int sys_fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_active()) return E_NOT_FOUND;
    LOG_LOCKED();   /* the console draws here too, under it */
    fb_fill_rect(x, y, w, h, fb_pack(color));
    return 0;
}

int sys_fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_active()) return E_NOT_FOUND;
    LOG_LOCKED();   /* the console draws here too, under it */
    fb_put_pixel(x, y, fb_pack(color));
    return 0;
}

int sys_fb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    if (!fb_active()) return E_NOT_FOUND;
    LOG_LOCKED();   /* the console draws here too, under it */
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
       SYS_CONSOLE_WRITE has always done this correctly; this now matches it.
       Drawn from a copy (1.9e4), a piece at a time, each ending at its
       page's end: a page that is not there ends the text, as the scan did,
       and the page after a NUL is never needed. */
    char piece[256];
    uint32_t n = 0;
    while (n < 0x1000) {
        uint32_t room = 0x1000u - (uint32_t)((text + n) & 0xFFFu);
        uint32_t m = room < sizeof piece ? room : (uint32_t)sizeof piece;
        if (m > 0x1000u - n) m = 0x1000u - n;
        if (user_copy_in(piece, text + n, m) != 0) {
            if (n == 0) return E_BAD_ADDRESS;
            break;
        }
        uint32_t k = 0;
        while (k < m && piece[k]) k++;
        log_lock();                          /* never across the copy above */
        fb_draw_string_n(x + n * font_cell_width(), y, piece, k, fb_pack(fg), fb_pack(bg));
        log_unlock();
        n += k;
        if (k < m) break;                    /* the NUL */
    }
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
    int len = user_copy_str(name, uname, sizeof name);      /* checked on the copy (1.9e4) */
    if (len == E_OUT_OF_RANGE) return E_INVALID_ARGS;       /* no terminator in range */
    if (len < 0) return len;
    for (uint32_t n = 0; n < (uint32_t)len; n++) {
        char c = name[n];
        /* Everything that could leave /System/Fonts is refused here, so
           nothing downstream has to reason about traversal. '.' goes
           too: the extension is ours to add, and without it ".." never
           needs a special case. */
        if (c == '/' || c == '\\' || c == '.') return E_INVALID_ARGS;
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) return E_INVALID_ARGS;
    }
    if (len == 0) return E_INVALID_ARGS;

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
    STORAGE_LOCKED();   /* the cell before and after is one selection's (1.9e4) */
    uint32_t cw = font_cell_width();
    uint32_t ch = font_cell_height();
    int rc = fb_font_select(name);
    if (rc != 0) return rc;
    if (font_cell_width() != cw || font_cell_height() != ch) console_clear();
    return 0;
}

/* ---- the screen given away (2.3) -------------------------------------------
 * SYS_SCREEN_OPEN: the framebuffer's pages as a KMEM_SCREEN memory object,
 * for the display server (docs/system/CX_DISPLAY.md section 3). The kernel
 * keeps drawing, into its copy alone, so the console a release shows is
 * current. Refused without a copy: then the kernel draws on the screen
 * itself and would draw over the holder. */
#include "object/kmem.h"
#include "object/htab.h"
#include "sched.h"

int sys_screen_open(uint64_t uinfo) {
    if (!fb_active()) return E_NOT_FOUND;
    if (!user_ptr_writable(uinfo, sizeof(struct screen_info))) return E_BAD_ADDRESS;
    if ((fb_phys & 0xFFFu) || back == fb) return E_NOT_SUPPORTED;
    uint32_t pages = (pitch * height + 0xFFFu) >> 12;
    struct screen_info si = { 0 };
    si.width  = width;
    si.height = height;
    si.pitch  = pitch;
    si.bpp    = bpp;
    si.format = bpp == 32 ? SCREEN_XRGB8888 : SCREEN_RGB565;
    si.size   = pages << 12;

    log_lock();
    int busy = held || taken;
    if (!busy) held = 1;
    log_unlock();
    if (busy) return E_BUSY;

    int rc;
    struct kmem *m = kmem_create_screen((paddr_t)fb_phys, pages, &rc);
    if (!m) { fb_screen_release(); return rc; }
    /* The object's last reference releases the screen, so every way out
       from here goes through it. */
    if (user_copy_out(uinfo, &si, sizeof si) != 0) { kobj_unref(&m->hdr); return E_BAD_ADDRESS; }
    int32_t h = htab_install(thread_htab(thread_current_id()), &m->hdr,
                             HRIGHT_MAP | HRIGHT_MAP_WRITE | HRIGHT_INSPECT | HRIGHT_DUPLICATE, 0);
    kobj_unref(&m->hdr);
    return h;
}

void fb_screen_release(void) {
    log_lock();
    held = 0;
    flush(0, 0, width, height);
    log_unlock();
}

void fb_panic_takeover(void) { taken = 1; }

int fb_screen_held(void) { return held && !taken; }

#if CXK_TEST_HOOKS
void fb_test_hold(void) { held = 1; }

void fb_test_untake(void) {
    log_lock();
    taken = 0;
    log_unlock();
}
#endif
