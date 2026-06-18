/* /CXK/kernel/drivers/char/console.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Minimal text-mode console on top of vga.c (see console.h). */

#include "console.h"
#include "vga.h"
#include "format.h"

#define TAB_WIDTH 4

/* console state */
static int     cx = 0;          /* cursor column */
static int     cy = 0;          /* cursor row */
static uint8_t attr = 0;        /* current text attribute */

/* scroll the screen up by one line when the cursor passes the bottom. We don't
   keep scrollback (v5 is a boot log, not an interactive shell); the top line is
   simply discarded. Reads/writes go through vga_put_cell so there's one path to
   VGA memory. */
static void scroll_if_needed(void) {
    if (cy < VGA_HEIGHT) return;

    /* move every row up one. vga.c doesn't expose a raw read, so we mirror its
       cell formula here against the same higher-half buffer. */
    volatile uint16_t *mem = (volatile uint16_t *)(0xC0000000 + 0xB8000);
    for (int y = 1; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            mem[(y - 1) * VGA_WIDTH + x] = mem[y * VGA_WIDTH + x];

    /* clear the last row to the current attribute */
    for (int x = 0; x < VGA_WIDTH; x++)
        vga_put_cell(x, VGA_HEIGHT - 1, ' ', attr);

    cy = VGA_HEIGHT - 1;
}

void console_init(void) {
    attr = VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK);
    cx = 0; cy = 0;
    vga_clear(attr);
    vga_set_cursor(0, 0);
}

void console_clear(void) {
    vga_clear(attr);
    cx = 0; cy = 0;
    vga_set_cursor(0, 0);
}

void console_set_color(uint8_t a) { attr = a; }
uint8_t console_get_color(void)   { return attr; }

void console_newline(void) {
    cx = 0;
    cy++;
    scroll_if_needed();
    vga_set_cursor(cx, cy);
}

void console_putc(char c) {
    if (c == '\n') { console_newline(); return; }
    if (c == '\r') { cx = 0; vga_set_cursor(cx, cy); return; }
    if (c == '\t') {
        int next = (cx / TAB_WIDTH + 1) * TAB_WIDTH;
        while (cx < next && cx < VGA_WIDTH) { vga_put_cell(cx, cy, ' ', attr); cx++; }
        if (cx >= VGA_WIDTH) console_newline();
        else vga_set_cursor(cx, cy);
        return;
    }

    vga_put_cell(cx, cy, c, attr);
    cx++;
    if (cx >= VGA_WIDTH) console_newline();
    else vga_set_cursor(cx, cy);
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
    console_puts("0x");
    fmt_hex(buf, v, width, 1);
    console_puts(buf);
}

void console_set_cursor(int x, int y) {
    if (x < 0) x = 0; 
    if (x >= VGA_WIDTH)  x = VGA_WIDTH - 1;
    if (y < 0) y = 0; 
    if (y >= VGA_HEIGHT) y = VGA_HEIGHT - 1;
    cx = x; cy = y;
    vga_set_cursor(cx, cy);
}

void console_get_cursor(int *x, int *y) {
    if (x) *x = cx;
    if (y) *y = cy;
}

/* ---- tagged log lines ---- */
void console_tag(const char *tag, uint8_t tag_attr, const char *msg) {
    uint8_t save = attr;
    console_puts_color("[", VGA_ATTR(VGA_DARK_GREY, VGA_BLACK));
    console_puts_color(tag, tag_attr);
    console_puts_color("] ", VGA_ATTR(VGA_DARK_GREY, VGA_BLACK));
    attr = save;
    console_puts(msg);
    console_newline();
}

void console_boot(const char *msg) {
    console_tag("BOOT",   VGA_ATTR(VGA_LIGHT_CYAN,  VGA_BLACK), msg);
}
void console_kernel(const char *msg) {
    console_tag("KERNEL", VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK), msg);
}
void console_warn(const char *msg) {
    console_tag("WARN",   VGA_ATTR(VGA_YELLOW,      VGA_BLACK), msg);
}
void console_err(const char *msg) {
    console_tag("ERR",    VGA_ATTR(VGA_LIGHT_RED,   VGA_BLACK), msg);
}