// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/lib/string/syslog.c */
/* Aurora Tejeda / CATX Systems */
/* Structured system logger - in-memory ring buffer recorder. See syslog.h. */

#include "syslog.h"
#include "console.h"
#include "color.h"

static struct slog_entry ring[SLOG_RING];
static uint32_t head = 0;     /* next write slot */
static uint32_t total = 0;    /* total recorded (>= retained) */
int slog_in_progress = 0;

static void copy_field(char *dst, int cap, const char *src) {
    int i = 0;
    if (src) for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void slog_record(const char *system, const char *subsystem, log_sev_t sev, const char *event) {
    struct slog_entry *e = &ring[head % SLOG_RING];
    e->seq = total;
    e->sev = (uint8_t)sev;
    copy_field(e->system,    SLOG_SYS_W, system);
    copy_field(e->subsystem, SLOG_SUB_W, subsystem);
    copy_field(e->event,     SLOG_EVT_W, event);
    head++;
    total++;
}

void slog(const char *system, const char *subsystem, log_sev_t sev, const char *event) {
    slog_record(system, subsystem, sev, event);
    slog_in_progress = 1;          /* klog mirrors to console; don't re-record */
    klog(system, sev, event);
    slog_in_progress = 0;
}

uint32_t slog_count(void) { return total < SLOG_RING ? total : SLOG_RING; }
uint32_t slog_total(void) { return total; }

const struct slog_entry *slog_get(uint32_t i) {
    uint32_t n = slog_count();
    if (i >= n) return 0;
    uint32_t start = (total < SLOG_RING) ? 0 : head;   /* oldest slot */
    return &ring[(start + i) % SLOG_RING];
}

void slog_dump_console(void) {
    uint32_t n = slog_count();
    console_puts_color("---- system log (oldest first) ----\n", VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK));
    for (uint32_t i = 0; i < n; i++) {
        const struct slog_entry *e = slog_get(i);
        if (!e) break;
        console_puts_color(e->system, VGA_ATTR(VGA_CYAN, VGA_BLACK));
        if (e->subsystem[0]) {
            console_puts_color("/", VGA_ATTR(VGA_DARK_GREY, VGA_BLACK));
            console_puts_color(e->subsystem, VGA_ATTR(VGA_CYAN, VGA_BLACK));
        }
        console_puts_color(": ", VGA_ATTR(VGA_DARK_GREY, VGA_BLACK));
        console_puts_color(e->event, VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK));
        console_newline();
    }
}

