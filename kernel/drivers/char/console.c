// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/char/console.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Text console with two interchangeable backends behind one interface:
 *   - VGA text  (vga.c, 80x25, hardware cursor) - the default at boot
 *   - framebuffer (fb.c, 8x16 glyphs in a pixel viewport) - after console_use_fb
 *
 * The 16-color VGA attribute model is identical for both backends; on the
 * framebuffer path the color INDEX is translated to RGB through vga_palette_rgb[]
 * (color.c), so "light green on black" looks right in graphics mode too.
 * Severity/tag logging built on these primitives lives in lib/string/logging.c.
 */

#include "console.h"
#include "vga.h"
#include "fb.h"
#include "color.h"
#include "format.h"
#include "serial.h"

#define TAB_WIDTH 4

/* console state */
static int     cx = 0;              /* cursor column (cells) */
static int     cy = 0;              /* cursor row (cells) */
static uint8_t attr = 0;            /* current VGA attribute (bg<<4 | fg) */

/* active backend + geometry */
static int     use_fb = 0;          /* 0 = VGA text, 1 = framebuffer */
static int     cols = VGA_WIDTH;    /* text columns */
static int     rows = VGA_HEIGHT;   /* text rows */

/* framebuffer viewport (pixels), valid only when use_fb */
static uint32_t vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;

/* ---- backend cell operations ---- */

/* VGA color index -> packed framebuffer pixel via the canonical RGB palette */
static uint32_t pal_fb(uint8_t idx) {
    uint32_t c = vga_palette_rgb[idx & 0x0F];
    return fb_rgb((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
}

static void be_put_cell(int x, int y, char c, uint8_t a) {
    if (use_fb) {
        uint32_t fg = pal_fb(a & 0x0F);
        uint32_t bg = pal_fb((a >> 4) & 0x0F);
        fb_draw_char(vp_x + (uint32_t)x * FB_CHAR_W,
                     vp_y + (uint32_t)y * FB_CHAR_H, c, fg, bg);
    } else {
        vga_put_cell(x, y, c, a);
    }
}

static void be_clear(uint8_t a) {
    if (use_fb) {
        fb_fill_rect(vp_x, vp_y, vp_w, vp_h, pal_fb((a >> 4) & 0x0F));
    } else {
        vga_clear(a);
    }
}

/* scroll the text area up one row, clearing the new bottom row to `a`'s bg */
static void be_scroll_one(uint8_t a) {
    if (use_fb) {
        fb_scroll_rect(vp_x, vp_y, vp_w, vp_h, FB_CHAR_H, pal_fb((a >> 4) & 0x0F));
    } else {
        /* mirror vga.c's cell formula against the higher-half text buffer */
        volatile uint16_t *mem = (volatile uint16_t *)(0xC0000000 + 0xB8000);
        for (int y = 1; y < VGA_HEIGHT; y++)
            for (int x = 0; x < VGA_WIDTH; x++)
                mem[(y - 1) * VGA_WIDTH + x] = mem[y * VGA_WIDTH + x];
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_put_cell(x, VGA_HEIGHT - 1, ' ', a);
    }
}

static void be_set_cursor(int x, int y) {
    if (use_fb) {
        /* no hardware cursor on the framebuffer; the boot log doesn't need a
           caret. (A software caret could be drawn here later.) */
        (void)x; (void)y;
    } else {
        vga_set_cursor(x, y);
    }
}

/* scroll the screen up by one line when the cursor passes the bottom. No
   scrollback (v5 is a boot log, not an interactive shell); the top line is
   discarded. */
static void scroll_if_needed(void) {
    if (cy < rows) return;
    be_scroll_one(attr);
    cy = rows - 1;
}

void console_init(void) {
    attr = VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK);
    use_fb = 0;
    cols = VGA_WIDTH;
    rows = VGA_HEIGHT;
    cx = 0; cy = 0;
    vga_clear(attr);
    vga_set_cursor(0, 0);
}

/* Switch the console onto the framebuffer, rendering into the pixel rectangle
   (x0,y0,w,h). Safe to call only after fb_init() succeeded; otherwise a no-op
   (stays in VGA text mode). Clears the viewport and homes the cursor. */
void console_use_fb(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    if (!fb_active()) return;
    use_fb = 1;
    vp_x = x0; vp_y = y0; vp_w = w; vp_h = h;
    cols = (int)(w / FB_CHAR_W);
    rows = (int)(h / FB_CHAR_H);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    cx = 0; cy = 0;
    be_clear(attr);
}

void console_clear(void) {
    be_clear(attr);
    cx = 0; cy = 0;
    be_set_cursor(0, 0);
}

void console_set_color(uint8_t a) { attr = a; }
uint8_t console_get_color(void)   { return attr; }

void console_newline(void) {
    /* The serial tee lives here as well as in console_putc because the loggers
       (lib/string/logging.c, syslog.c) call console_newline() directly rather
       than printing '\n'. Teeing only in console_putc would run every log line
       together in the serial capture. console_putc delegates '\n' to this
       function and tees everything else, so each byte is sent exactly once. */
    serial_putc('\r');
    serial_putc('\n');

    cx = 0;
    cy++;
    scroll_if_needed();
    be_set_cursor(cx, cy);
}

void console_putc(char c) {
    if (c == '\n') { console_newline(); return; }   /* tees there, not here */

    serial_putc(c);

    if (c == '\r') { cx = 0; be_set_cursor(cx, cy); return; }
    if (c == '\b') {
        /* Move the cursor back one cell, wrapping to the end of the previous
           line. Does NOT erase - callers that want to erase send "\b \b"
           (back, overwrite with a space, back again), which is what the shell's
           read_line does. Without this case the 0x08 byte was drawn as a glyph. */
        if (cx > 0)      { cx--; }
        else if (cy > 0) { cy--; cx = cols - 1; }
        be_set_cursor(cx, cy);
        return;
    }
    if (c == '\t') {
        int next = (cx / TAB_WIDTH + 1) * TAB_WIDTH;
        while (cx < next && cx < cols) { be_put_cell(cx, cy, ' ', attr); cx++; }
        if (cx >= cols) console_newline();
        else be_set_cursor(cx, cy);
        return;
    }

    be_put_cell(cx, cy, c, attr);
    cx++;
    if (cx >= cols) console_newline();
    else be_set_cursor(cx, cy);
}

void console_puts(const char *s) {
    while (*s) console_putc(*s++);
}

void console_puts_color(const char *s, uint8_t a) {
    uint8_t save = attr;
    attr = a;
    console_puts(s);
    attr = save;
}

void console_put_u32(uint32_t v) {
    char buf[11];
    fmt_u32(buf, v);
    console_puts(buf);
}

void console_put_i32(int32_t v) {
    char buf[12];
    fmt_i32(buf, v);
    console_puts(buf);
}

void console_put_hex(uint32_t v, int width) {
    char buf[9];
    if (width > 8) width = 8;     /* u32 is 8 hex digits max; guard buf[9] */
    console_puts("0x");
    fmt_hex(buf, v, width, 1);
    console_puts(buf);
}

/* ---- colored-number helpers (value in its own color, current restored) ---- */
void console_put_u32_color(uint32_t v, uint8_t a) {
    uint8_t save = attr; attr = a; console_put_u32(v); attr = save;
}
void console_put_i32_color(int32_t v, uint8_t a) {
    uint8_t save = attr; attr = a; console_put_i32(v); attr = save;
}
void console_put_hex_color(uint32_t v, int width, uint8_t a) {
    uint8_t save = attr; attr = a; console_put_hex(v, width); attr = save;
}

/* ---- label + value helpers (label in current color, value colored) ---- */
void console_field_u32(const char *label, uint32_t v, uint8_t value_attr) {
    console_puts(label);
    console_put_u32_color(v, value_attr);
}
void console_field_i32(const char *label, int32_t v, uint8_t value_attr) {
    console_puts(label);
    console_put_i32_color(v, value_attr);
}
void console_field_hex(const char *label, uint32_t v, int width, uint8_t value_attr) {
    console_puts(label);
    console_put_hex_color(v, width, value_attr);
}

void console_set_cursor(int x, int y) {
    if (x < 0) x = 0;
    if (x >= cols) x = cols - 1;
    if (y < 0) y = 0;
    if (y >= rows) y = rows - 1;
    cx = x; cy = y;
    be_set_cursor(cx, cy);
}

void console_get_cursor(int *x, int *y) {
    if (x) *x = cx;
    if (y) *y = cy;
}