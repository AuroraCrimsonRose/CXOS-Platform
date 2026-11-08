// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/panicscreen.c */
/* Aurora Tejeda / CATX Systems */
#include "diag/panicscreen.h"
#include "diag/crashlog.h"
#include "config.h"
#include "fb.h"
#include "arch/smp.h"
#include "cpu/percpu.h"

static char    qr_text[PANIC_QR_BYTES_MAX];
static char    log_tail[1024];
static uint8_t qr_code[PANIC_QR_BUF_LEN], qr_tmp[PANIC_QR_BUF_LEN];

/* The pen, kept so panic_screen_status can add its line under the report. */
static uint32_t pen_x, pen_y, pen_cols, pen_fg, pen_bg;
static int pen_ready;

static void text_at(uint32_t row_y, const char *s, uint32_t max) {
    uint32_t fw = fb_font_width();
    for (uint32_t i = 0; i < max && s[i]; i++)
        fb_draw_char(pen_x + i * fw, row_y, s[i], pen_fg, pen_bg);
}

static void line(const char *s) {
    if (pen_y + fb_font_height() <= fb_height()) text_at(pen_y, s, pen_cols);
    pen_y += fb_font_height();
}

/* Long text, broken at a space where there is one. */
static void wrapped(const char *s, unsigned rows) {
    while (*s && rows--) {
        uint32_t n = 0, cut = 0;
        while (s[n] && n < pen_cols) { if (s[n] == ' ') cut = n; n++; }
        if (!s[n] || cut == 0) cut = n;
        if (pen_y + fb_font_height() <= fb_height()) text_at(pen_y, s, cut);
        pen_y += fb_font_height();
        s += cut;
        if (*s == ' ') s++;
    }
}

static void gap(void) { pen_y += fb_font_height() / 2; }

static char *put_s(char *p, char *end, const char *s) {
    while (s && *s && p < end) *p++ = *s++;
    return p;
}

static char *put_hex(char *p, char *end, uint64_t v, int digits) {
    for (int i = (digits - 1) * 4; i >= 0 && p < end; i -= 4) *p++ = "0123456789ABCDEF"[(v >> i) & 0xF];
    return p;
}

static void draw_code(unsigned modules, const struct panic_layout *l) {
    fb_fill_rect(l->qr_x, l->qr_y, l->qr_side, l->qr_side, fb_rgb(255, 255, 255));
    uint32_t base_x = l->qr_x + PANIC_QR_QUIET * l->px;
    uint32_t base_y = l->qr_y + PANIC_QR_QUIET * l->px;
    uint32_t dark = fb_rgb(0, 0, 0);
    for (unsigned y = 0; y < modules; y++)
        for (unsigned x = 0; x < modules; x++)
            if (panic_qr_module(qr_code, x, y))
                fb_fill_rect(base_x + x * l->px, base_y + y * l->px, l->px, l->px, dark);
}

void panic_screen_show(struct panic_report *r) {
    if (!fb_active()) return;

    uint32_t n = crashlog_log_tail(log_tail, sizeof log_tail - 1);
    log_tail[n] = 0;
    r->log = log_tail;

    size_t len = panic_qr_text(r, qr_text, sizeof qr_text);
    unsigned modules = panic_qr_encode(qr_text, len, qr_code, qr_tmp);

    struct panic_layout l;
    uint32_t fw = fb_font_width(), fh = fb_font_height();
    panic_layout_for(fb_width(), fb_height(), fw, fh, modules, &l);

    pen_x = fw * 2;
    pen_y = fh;
    pen_cols = l.text_cols;
    pen_fg = fb_rgb(255, 255, 255);
    pen_bg = fb_rgb(0xAA, 0, 0);

    line("CXOS has stopped.");
    gap();
    wrapped(panic_plain(r->stop_code), 2);
    wrapped(panic_advice(l.px != 0), 2);
    gap();

    char buf[96], *p, *end = buf + sizeof buf - 1;

    p = put_s(buf, end, "Stop code: ");
    p = put_s(p, end, r->stop_name);
    p = put_s(p, end, " (CX-");
    p = put_hex(p, end, r->stop_code, 4);
    p = put_s(p, end, ")");
    *p = 0;
    line(buf);

    p = put_s(buf, end, "Crash number: ");
    p = put_hex(p, end, r->crash_id, 8);
    *p = 0;
    line(buf);
    gap();

    wrapped(r->why ? r->why : "(no reason given)", 3);
    gap();

    line("In:");
    for (unsigned i = 0; i < r->nframes && i < PANIC_QR_FRAMES; i++) {
        p = put_s(buf, end, "  ");
        if (r->frames[i].name) {
            p = put_s(p, end, r->frames[i].name);
            p = put_s(p, end, "+0x");
            p = put_hex(p, end, r->frames[i].off, 4);
        } else {
            p = put_hex(p, end, r->frames[i].addr, 16);
        }
        *p = 0;
        line(buf);
    }
    gap();
    pen_ready = 1;

    if (l.px) draw_code(modules, &l);
}

void panic_screen_status(const char *s) {
    if (pen_ready) line(s);
}

/* ---- the crash test ---- */

#if CXK_TEST_HOOKS
/* Paints over the half of the screen the code goes in until the panic stops
   this CPU, as a display server on another CPU would: without the stop, the
   code a phone is meant to read is scribbled on. Runs in an AP's idle thread
   with interrupts off, so it is smp_serve that notices. */
static void test_paint(void *arg) {
    (void)arg;
    uint32_t w = fb_width(), h = fb_height(), ink = fb_rgb(0xFF, 0, 0xFF);
    for (;;)
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = w / 2; x < w; x += 3) fb_test_screen_poke(x, y, ink);
            smp_serve();
        }
}

void panic_test_prepare(void) {
    if (!fb_active()) return;
    fb_test_hold();
    if (cpu_at(1)) (void)smp_call_start(1, test_paint, 0);
}
#else
void panic_test_prepare(void) {}
#endif
