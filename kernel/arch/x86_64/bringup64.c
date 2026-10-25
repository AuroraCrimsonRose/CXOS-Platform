// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/bringup64.c */
/* Aurora Tejeda / CATX Systems */
/* Three generic interfaces the x86-64 kernel calls before the code that owns
   them runs here, each the smallest thing that keeps the caller honest:

   - logging.h, on COM1 through early_con. The real logging.c drives the
     console, which needs the framebuffer or VGA text, and the log ring.
   - platform_tick_start, the one part of the PC platform (platform/pc/
     platform.c) the timer needs: the PIT. The rest of platform.c brings the
     PS/2 devices, and with them the mouse and USB HID.
   - keyboard_tick, which wakes a USB keyboard reader; there is none yet.

   Listed in docs/planning/TRANSITIONAL_CODE.md; each goes when its owner is
   built for x86-64 (step 5f, when the drivers run here). */

#include "early_con.h"
#include "logging.h"
#include "keyboard.h"
#include "platform/platform.h"
#include "platform/pc/pit.h"

/* ---- logging.h ---- */

static const char *const sev_name[] = { "INFO", "OK  ", "WARN", "ERR ", "FAIL" };

static int quiet;

static const char *sev_of(log_sev_t sev) {
    return (unsigned)sev < sizeof(sev_name) / sizeof(sev_name[0]) ? sev_name[sev] : sev_name[0];
}

/* klog's child lines indent to its message column: "[" 8 "] - [" 4 "] - " */
static void child_indent(void) {
    for (int i = 0; i < 1 + LOG_TAG_W + 5 + LOG_SEV_W + 4; i++) early_putc(' ');
}

void klog(const char *tag, log_sev_t sev, const char *msg) {
    early_log(tag, sev_of(sev), msg ? msg : "");
}

/* There is no screen yet, so a line meant only for the serial port is a klog. */
void klog_offscreen(const char *tag, log_sev_t sev, const char *msg) {
    klog(tag, sev, msg);
}

void klog_u32(const char *tag, log_sev_t sev,
              const char *label, uint32_t value, uint8_t value_attr,
              const char *suffix) {
    (void)value_attr;
    early_log_start(tag, sev_of(sev));
    if (label) early_puts(label);
    early_dec(value);
    if (suffix) early_puts(suffix);
    early_puts("\n");
}

void klog_child(const char *msg) {
    child_indent();
    early_puts(msg ? msg : "");
    early_puts("\n");
}

void klog_child_u32(const char *label, uint32_t value, uint8_t value_attr,
                    const char *suffix) {
    (void)value_attr;
    child_indent();
    if (label) early_puts(label);
    early_dec(value);
    if (suffix) early_puts(suffix);
    early_puts("\n");
}

void klog_quiet(void) { quiet = 1; }
int  klog_is_quiet(void) { return quiet; }

/* ---- platform/platform.h: the tick ---- */

void platform_tick_start(uint32_t hz, irq_handler_t on_tick) {
    pit_start_periodic(hz, on_tick);
}

/* ---- keyboard.h ---- */

void keyboard_tick(void) { }
