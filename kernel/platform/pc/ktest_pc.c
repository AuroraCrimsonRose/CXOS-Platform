// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/ktest_pc.c */
/* Aurora Tejeda / CATX Systems */
/* platform/ktest_platform.h on a PC: the boot self-tests that drive the PC's
   own devices, kept here because generic code may not name them. */

#include <stdint.h>
#include "platform/ktest_platform.h"
#include "i8042.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "logging.h"

/* ---- the PS/2 keyboard and mouse reach the kernel's input ----
 *
 * The 8042 will put a byte in its output buffer as though the keyboard (0xD2)
 * or the mouse (0xD3) had sent it, and raise the same interrupt. That drives
 * the whole path a key takes - the controller, IRQ 1 or 12 through whichever
 * interrupt controller is live, the decoder, the buffer - with nobody at the
 * keyboard. A machine with no 8042 (reads all ones) is untested, with a WARN. */
static int ps2_send(uint8_t cmd, uint8_t byte) {
    i8042_command(cmd);
    i8042_write(byte);
    /* Wait for the interrupt handler to take it. Real hardware latches the
       byte a little after the write, so an empty buffer means "taken" only
       once two ticks have passed. */
    uint32_t t0 = timer_ticks();
    while (timer_ticks() - t0 < 100)
        if (!(i8042_status() & I8042_STAT_OUTPUT_FULL) && timer_ticks() - t0 > 1) return 1;
    return 0;
}

int ktest_platform_input(void) {
    if (i8042_status() == 0xFF) {
        klog("KTEST", SEV_WARN, "PS/2 input untested: no 8042");
        return 1;
    }
    while (keyboard_getchar()) { }                         /* drain */

    /* 'a' pressed and released: one key, and the release none */
    if (!ps2_send(I8042_CMD_WRITE_KBD_OUT, 0x1E) || !ps2_send(I8042_CMD_WRITE_KBD_OUT, 0x9E)) {
        klog("KTEST", SEV_FAIL, "PS/2: the controller never handed over a keyboard byte");
        return 0;
    }
    char c1 = keyboard_getchar(), c2 = keyboard_getchar();
    if (c1 != 'a' || c2 != 0) {
        klog("KTEST", SEV_FAIL, "PS/2: a key sent through the 8042 did not arrive as 'a'");
        return 0;
    }

    if (!mouse_present()) {
        klog("KTEST", SEV_WARN, "PS/2 mouse untested: none attached");
        return 1;
    }
    /* Right 5 and up 3, then left 5 and down 3, which uses the 9-bit signs.
       PS/2 Y is positive-up and the screen's is positive-down, so "up 3" is
       3 rows nearer the top. The cursor starts centred, clear of the clamp. */
    int32_t x0 = mouse_x(), y0 = mouse_y();
    static const uint8_t pkt[2][3] = { { 0x08, 5, 3 }, { 0x38, 0xFB, 0xFD } };
    static const int32_t want_dx[2] = { 5, 0 }, want_dy[2] = { -3, 0 };
    for (int p = 0; p < 2; p++) {
        uint32_t seq0 = mouse_seq();
        for (int i = 0; i < 3; i++)
            if (!ps2_send(I8042_CMD_WRITE_AUX_OUT, pkt[p][i])) {
                klog("KTEST", SEV_FAIL, "PS/2: the controller never handed over a mouse byte");
                return 0;
            }
        if (mouse_seq() - seq0 != 1) {
            klog("KTEST", SEV_FAIL, "PS/2: a mouse packet was not one movement");
            return 0;
        }
        if (mouse_x() != x0 + want_dx[p] || mouse_y() != y0 + want_dy[p]) {
            klog("KTEST", SEV_FAIL, "PS/2: a mouse packet moved the pointer wrongly");
            return 0;
        }
    }
    return 1;
}
