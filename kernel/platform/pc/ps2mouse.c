// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/ps2mouse.c */
/* Aurora Tejeda / CATX Systems */
/*
 * PS/2 mouse on the 8042 controller - the same chip the keyboard already uses,
 * on IRQ12 instead of IRQ1.
 *
 * The mouse reports RELATIVE movement in 3-byte packets, which this driver
 * hands to mouse.c; mouse.c keeps the absolute cursor position.
 *
 * Packet format (standard 3-byte, no scroll wheel):
 *   byte 0: YO XO YS XS 1 MB RB LB   overflow, sign, always-1, buttons
 *   byte 1: X movement (signed, sign bit is XS in byte 0)
 *   byte 2: Y movement (signed, YS in byte 0) - POSITIVE IS UP, so it is negated
 *
 * The always-1 bit in byte 0 is used to resynchronise if a byte is ever lost.
 */

#include <stdint.h>
#include "ps2mouse.h"
#include "mouse.h"
#include "i8042.h"
#include "pic.h"
#include "arch/irq.h"

/* mouse commands */
#define MOUSE_SET_DEFAULTS   0xF6
#define MOUSE_ENABLE_REPORT  0xF4
#define MOUSE_ACK            0xFA

static uint8_t  packet[3];
static int      packet_idx;

static void mouse_write(uint8_t v) {
    i8042_command(I8042_CMD_TO_AUX);
    i8042_write(v);
}

/* ---- IRQ12 ---- */

static void ps2mouse_irq(struct registers *r) {
    (void)r;

    /* only consume bytes the controller says came from the mouse */
    uint8_t status = i8042_status();
    if (!(status & I8042_STAT_OUTPUT_FULL) || !(status & I8042_STAT_FROM_AUX)) return;

    uint8_t b = i8042_data();

    /* byte 0 always has bit 3 set; if it doesn't we've lost sync - drop it */
    if (packet_idx == 0 && !(b & 0x08)) return;

    packet[packet_idx++] = b;
    if (packet_idx < 3) return;
    packet_idx = 0;

    uint8_t flags = packet[0];

    /* discard packets whose movement overflowed - the deltas are meaningless */
    if (flags & 0xC0) return;

    int32_t dx = (int32_t)packet[1];
    int32_t dy = (int32_t)packet[2];
    if (flags & 0x10) dx |= (int32_t)0xFFFFFF00;   /* sign-extend from 9 bits */
    if (flags & 0x20) dy |= (int32_t)0xFFFFFF00;

    /* PS/2 reports Y positive-UP; screens are positive-down, so negate here and
       let mouse.c work in screen direction only. USB HID already
       reports positive-down and needs no such correction. */
    mouse_inject(dx, -dy, (uint32_t)(flags & 0x07));
}

/* ---- public ---- */

int ps2mouse_init(void) {
    /* enable the auxiliary (mouse) port */
    i8042_command(I8042_CMD_ENABLE_AUX);

    /* turn on IRQ12 in the controller config byte */
    i8042_command(I8042_CMD_READ_CFG);
    uint8_t cfg = i8042_read();
    cfg |= 0x02;            /* enable the mouse interrupt */
    cfg &= (uint8_t)~0x20;  /* clear "mouse clock disabled" */
    i8042_command(I8042_CMD_WRITE_CFG);
    i8042_write(cfg);

    /* defaults, then start streaming - each command is ACKed */
    mouse_write(MOUSE_SET_DEFAULTS);
    if (i8042_read() != MOUSE_ACK) return 0;
    mouse_write(MOUSE_ENABLE_REPORT);
    if (i8042_read() != MOUSE_ACK) return 0;

    packet_idx = 0;
    irq_install_handler(12, ps2mouse_irq);
    /* Unmask AFTER the handler is recorded, never before: an IRQ arriving in the
       gap would find irq_routines[12] still null and the packet would be lost,
       leaving the byte stream out of sync. Firmware need not enable IRQ12, and
       a line left masked means the handler is installed and never called.
       SeaBIOS happens to leave it open (the masks here are 0x8EB8 under QEMU),
       so no boot under QEMU fails without this line: the PS/2 self-test was
       seen to pass with it deleted. */
    pic_unmask(12);
    mouse_attach();
    return 1;
}
