/* /kernel/lib/string/logging.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Column-aligned severity logging on top of the console. See logging.h. */

#include "logging.h"
#include "syslog.h"
#include "console.h"
#include "color.h"

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

/* write `s` left-aligned in `width` cols (truncated if longer), in `attr` */
static void put_field(const char *s, int width, uint8_t attr) {
    int len = slen(s);
    if (len > width) len = width;
    uint8_t save = console_get_color();
    console_set_color(attr);
    for (int i = 0; i < len;   i++) console_putc(s[i]);
    for (int i = len; i < width; i++) console_putc(' ');   /* pad on the right */
    console_set_color(save);
}

static void prefix(const char *tag, log_sev_t sev) {
    if ((int)sev < 0 || (int)sev >= SEV_COUNT) sev = SEV_INFO;
    console_puts_color("[", LOG_SEP);
    put_field(tag, LOG_TAG_W, LOG_TAG_COL);
    console_puts_color("] - [", LOG_SEP);
    put_field(sev_tbl[sev].name, LOG_SEV_W, sev_tbl[sev].attr);
    console_puts_color("] - ", LOG_SEP);
}

void klog(const char *tag, log_sev_t sev, const char *msg) {
    prefix(tag, sev);
    if (msg) console_puts_color(msg, LOG_MSG_COL);
    console_newline();
    if (!slog_in_progress) slog_record(tag, "", sev, msg);
}

void klog_u32(const char *tag, log_sev_t sev,
              const char *label, uint32_t value, uint8_t value_attr,
              const char *suffix) {
    prefix(tag, sev);
    if (label)  console_puts_color(label, LOG_MSG_COL);
    console_put_u32_color(value, value_attr);
    if (suffix) console_puts_color(suffix, LOG_MSG_COL);
    console_newline();
    if (!slog_in_progress) slog_record(tag, "", sev, label ? label : "");
}

/* indent + arrow so child content aligns under the message column */
static void child_indent(void) {
    uint8_t save = console_get_color();
    console_set_color(LOG_SEP);
    for (int i = 0; i < LOG_PREFIX_W - 3; i++) console_putc(' ');
    console_puts("-> ");
    console_set_color(save);
}

void klog_child(const char *msg) {
    child_indent();
    if (msg) console_puts_color(msg, LOG_MSG_COL);
    console_newline();
}

void klog_child_u32(const char *label, uint32_t value, uint8_t value_attr,
                    const char *suffix) {
    child_indent();
    if (label)  console_puts_color(label, LOG_MSG_COL);
    console_put_u32_color(value, value_attr);
    if (suffix) console_puts_color(suffix, LOG_MSG_COL);
    console_newline();
}