// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/platform.c */
/* Aurora Tejeda / CATX Systems */
/* platform/platform.h on a PC: the PIT ticks, the 8042 resets, the emulators'
   ports power off, the CMOS clock keeps the date, VGA text is the boot screen,
   and the PS/2 keyboard and mouse sit on the 8042. */

#include "platform/platform.h"
#include "emupower.h"
#include "i8042.h"
#include "pit.h"
#include "ps2kbd.h"
#include "ps2mouse.h"
#include "rtc.h"
#include "speaker.h"
#include "vga.h"
#include "logging.h"

void platform_init(void) {
    speaker_init();
}

void platform_tick_start(uint32_t hz, irq_handler_t on_tick) {
    pit_start_periodic(hz, on_tick);
}

/* At whichever controller routes IRQ 0 (irq_mask picks it). */
void platform_tick_stop(void)   { irq_mask(0); }
void platform_tick_resume(void) { irq_unmask(0); }

void platform_input_init(void) {
    ps2kbd_init();
    klog("INPUT", SEV_OK, "PS/2 keyboard online (IRQ1)");
    if (ps2mouse_init())
        klog("INPUT", SEV_OK, "PS/2 mouse online (IRQ12)");
    else
        klog("INPUT", SEV_WARN, "no PS/2 mouse detected");
}

void platform_reset(void)    { i8042_pulse_reset(); }
void platform_poweroff(void) { emu_poweroff(); }

int platform_wallclock(struct datetime *out) {
    struct rtc_time t;
    rtc_read(&t);
    out->year = t.year;  out->month  = t.month;  out->day    = t.day;
    out->hour = t.hour;  out->minute = t.minute; out->second = t.second;
    return 0;
}

int  platform_text_cols(void)                           { return VGA_WIDTH; }
int  platform_text_rows(void)                           { return VGA_HEIGHT; }
void platform_text_put(int x, int y, char c, uint8_t a) { vga_put_cell(x, y, c, a); }
void platform_text_clear(uint8_t attr)                  { vga_clear(attr); }
void platform_text_scroll(uint8_t attr)                 { vga_scroll_up(attr); }
void platform_text_cursor(int x, int y)                 { vga_set_cursor(x, y); }
