// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/string/logging.c */
/* Aurora Tejeda / CATX Systems */
/* Column-aligned severity logging on top of the console. See logging.h. */

#include "logging.h"
#include "syslog.h"
#include "console.h"
#include "color.h"
#include "serial.h"
#include "format.h"

#define LOG_SEP      VGA_ATTR(VGA_DARK_GREY,  VGA_BLACK)  /* brackets + " - " */
#define LOG_TAG_COL  VGA_ATTR(VGA_LIGHT_CYAN, VGA_BLACK)  /* the tag text */
#define LOG_MSG_COL  VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK)  /* the message text */

/* severity name + color, indexed by log_sev_t (keep order in sync) */
static const struct { const char *name; uint8_t attr; } sev_tbl[] = {
    { "INFO", VGA_ATTR(VGA_LIGHT_BLUE,  VGA_BLACK) },  /* SEV_INFO */
    { "OK",   VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK) },  /* SEV_OK   */
    { "WARN", VGA_ATTR(VGA_YELLOW,      VGA_BLACK) },  /* SEV_WARN */
    { "ERR",  VGA_ATTR(VGA_LIGHT_RED,   VGA_BLACK) },  /* SEV_ERR  */
    { "FAIL", VGA_ATTR(VGA_LIGHT_RED,   VGA_BLACK) },  /* SEV_FAIL */
};
#define SEV_COUNT (int)(sizeof(sev_tbl) / sizeof(sev_tbl[0]))

/* width of the whole "[tag] - [SEV] - " prefix; child lines indent to here */
#define LOG_PREFIX_W  (1 + LOG_TAG_W + 5 + LOG_SEV_W + 4)   /* "[" "] - [" "] - " */

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

/* Where a line goes. The screen belongs to the boot until a foreground program
   writes to it; after that a line below SEV_ERR goes to the serial port and the
   ring only, so the kernel stops printing over the shell's prompt. An error
   still reaches the screen - a fault is worth interrupting for. `screen` is the
   choice made for the last klog line, which the child lines under it follow. */
static int quiet;
static int screen = 1;

void klog_quiet(void) { quiet = 1; }
int  klog_is_quiet(void) { return quiet; }

static void out_s(const char *s, uint8_t attr) {
    if (screen) console_puts_color(s, attr);
    else        serial_puts(s);
}

static void out_u32(uint32_t v, uint8_t attr) {
    if (screen) { console_put_u32_color(v, attr); return; }
    char b[12];
    b[fmt_u32(b, v)] = 0;
    serial_puts(b);
}

static void out_nl(void) {
    if (screen) console_newline();
    else        { serial_putc('\r'); serial_putc('\n'); }
}

/* write `s` left-aligned in `width` cols (truncated if longer), in `attr` */
static void put_field(const char *s, int width, uint8_t attr) {
    char b[16];
    int len = slen(s);
    if (len > width) len = width;
    if (width > (int)sizeof b - 1) width = (int)sizeof b - 1;
    for (int i = 0; i < width; i++) b[i] = i < len ? s[i] : ' ';   /* pad on the right */
    b[width] = 0;
    out_s(b, attr);
}

/* The last line's tag and severity: its child lines go into the ring under
   them, so the saved log (cxk rig bootlog) keeps the detail the screen shows. */
static char      last_tag[LOG_TAG_W + 1];
static log_sev_t last_sev;

static void prefix(const char *tag, log_sev_t sev, int on_screen) {
    if ((int)sev < 0 || (int)sev >= SEV_COUNT) sev = SEV_INFO;
    screen = on_screen;
    int t = 0;
    for (; tag && tag[t] && t < LOG_TAG_W; t++) last_tag[t] = tag[t];
    last_tag[t] = 0;
    last_sev = sev;
    out_s("[", LOG_SEP);
    put_field(tag, LOG_TAG_W, LOG_TAG_COL);
    out_s("] - [", LOG_SEP);
    put_field(sev_tbl[sev].name, LOG_SEV_W, sev_tbl[sev].attr);
    out_s("] - ", LOG_SEP);
}

void klog(const char *tag, log_sev_t sev, const char *msg) {
    prefix(tag, sev, !quiet || sev >= SEV_ERR);
    if (msg) out_s(msg, LOG_MSG_COL);
    out_nl();
    if (!slog_in_progress) slog_record(tag, "", sev, msg);
}

/* klog without the screen, whatever the quiet state: a background process's
   line, which was never meant to be seen there. */
void klog_offscreen(const char *tag, log_sev_t sev, const char *msg) {
    prefix(tag, sev, 0);
    if (msg) out_s(msg, LOG_MSG_COL);
    out_nl();
    if (!slog_in_progress) slog_record(tag, "", sev, msg);
}

void klog_u32(const char *tag, log_sev_t sev,
              const char *label, uint32_t value, uint8_t value_attr,
              const char *suffix) {
    prefix(tag, sev, !quiet || sev >= SEV_ERR);
    if (label)  out_s(label, LOG_MSG_COL);
    out_u32(value, value_attr);
    if (suffix) out_s(suffix, LOG_MSG_COL);
    out_nl();

    /* The ring gets the whole line, not just the label: read back with
       SYS_LOG_READ, "pid " alone says nothing. */
    if (!slog_in_progress) {
        char line[SLOG_EVT_W];
        int n = 0;
        for (const char *p = label; p && *p && n < SLOG_EVT_W - 1; p++) line[n++] = *p;
        char num[12];
        num[fmt_u32(num, value)] = 0;
        for (const char *p = num; *p && n < SLOG_EVT_W - 1; p++) line[n++] = *p;
        for (const char *p = suffix; p && *p && n < SLOG_EVT_W - 1; p++) line[n++] = *p;
        line[n] = 0;
        slog_record(tag, "", sev, line);
    }
}

/* indent + arrow so child content aligns under the message column */
static void child_indent(void) {
    char b[LOG_PREFIX_W + 1];
    int n = 0;
    while (n < LOG_PREFIX_W - 3) b[n++] = ' ';
    b[n++] = '-'; b[n++] = '>'; b[n++] = ' ';
    b[n] = 0;
    out_s(b, LOG_SEP);
}

/* A child line into the ring, under the tag and severity of the line above
   it: "-> " then `a`, the value if there is one, then `b`. */
static void child_record(const char *a, int has_value, uint32_t value, const char *b) {
    if (slog_in_progress) return;
    char line[SLOG_EVT_W];
    int n = 0;
    for (const char *p = "-> "; *p; p++) line[n++] = *p;
    for (const char *p = a; p && *p && n < SLOG_EVT_W - 1; p++) line[n++] = *p;
    if (has_value) {
        char num[12];
        num[fmt_u32(num, value)] = 0;
        for (const char *p = num; *p && n < SLOG_EVT_W - 1; p++) line[n++] = *p;
    }
    for (const char *p = b; p && *p && n < SLOG_EVT_W - 1; p++) line[n++] = *p;
    line[n] = 0;
    slog_record(last_tag, "", last_sev, line);
}

void klog_child(const char *msg) {
    child_indent();
    if (msg) out_s(msg, LOG_MSG_COL);
    out_nl();
    child_record(msg, 0, 0, 0);
}

void klog_child_u32(const char *label, uint32_t value, uint8_t value_attr,
                    const char *suffix) {
    child_indent();
    if (label)  out_s(label, LOG_MSG_COL);
    out_u32(value, value_attr);
    if (suffix) out_s(suffix, LOG_MSG_COL);
    out_nl();
    child_record(label, 1, value, suffix);
}
