/* /CXK/kernel/drivers/input/mouse.c */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * PS/2 mouse on the 8042 controller - the same chip the keyboard already uses,
 * on IRQ12 instead of IRQ1.
 *
 * The mouse reports RELATIVE movement in 3-byte packets; this driver keeps an
 * absolute cursor position clamped to the screen so a GUI can just read x/y.
 *
 * Packet format (standard 3-byte, no scroll wheel):
 *   byte 0: YO XO YS XS 1 MB RB LB   overflow, sign, always-1, buttons
 *   byte 1: X movement (signed, sign bit is XS in byte 0)
 *   byte 2: Y movement (signed, YS in byte 0) - POSITIVE IS UP, so it is negated
 *
 * The always-1 bit in byte 0 is used to resynchronise if a byte is ever lost.
 */

#include <stdint.h>
#include "mouse.h"
#include "../../cpu/io.h"
#include "../../cpu/int/idt.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define PS2_STAT_OUTPUT_FULL 0x01
#define PS2_STAT_INPUT_FULL  0x02
#define PS2_STAT_FROM_MOUSE  0x20

/* controller commands */
#define PS2_CMD_ENABLE_AUX   0xA8   /* enable the second (mouse) port */
#define PS2_CMD_READ_CFG     0x20
#define PS2_CMD_WRITE_CFG    0x60
#define PS2_CMD_TO_MOUSE     0xD4   /* next byte goes to the mouse, not the kbd */

/* mouse commands */
#define MOUSE_SET_DEFAULTS   0xF6
#define MOUSE_ENABLE_REPORT  0xF4
#define MOUSE_ACK            0xFA

static int      present;
static int32_t  mx, my;                 /* absolute position, clamped */
static uint32_t buttons;                /* bit 0 left, 1 right, 2 middle */
static int32_t  clamp_w = 640, clamp_h = 480;

static uint8_t  packet[3];
static int      packet_idx;
static uint32_t moved;                  /* bumped on every state change */

/* ---- low-level 8042 helpers ---- */

static void wait_input(void) {          /* wait until we may write */
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & PS2_STAT_INPUT_FULL)) return;
}

static void wait_output(void) {         /* wait until there is a byte to read */
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & PS2_STAT_OUTPUT_FULL) return;
}

static void mouse_write(uint8_t v) {
    wait_input(); outb(PS2_CMD, PS2_CMD_TO_MOUSE);
    wait_input(); outb(PS2_DATA, v);
}

static uint8_t mouse_read(void) {
    wait_output();
    return inb(PS2_DATA);
}

/* ---- IRQ12 ---- */

static void mouse_callback(struct registers *r) {
    (void)r;

    /* only consume bytes the controller says came from the mouse */
    uint8_t status = inb(PS2_STATUS);
    if (!(status & PS2_STAT_OUTPUT_FULL) || !(status & PS2_STAT_FROM_MOUSE)) return;

    uint8_t b = inb(PS2_DATA);

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

    mx += dx;
    my -= dy;                            /* PS/2 Y is positive-up; screens are positive-down */

    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > clamp_w - 1) mx = clamp_w - 1;
    if (my > clamp_h - 1) my = clamp_h - 1;

    buttons = (uint32_t)(flags & 0x07);
    moved++;
}

/* ---- public ---- */

int mouse_init(int screen_w, int screen_h) {
    if (screen_w  > 0) clamp_w = screen_w;
    if (screen_h > 0) clamp_h = screen_h;
    mx = clamp_w / 2;
    my = clamp_h / 2;

    /* enable the auxiliary (mouse) port */
    wait_input(); outb(PS2_CMD, PS2_CMD_ENABLE_AUX);

    /* turn on IRQ12 in the controller config byte */
    wait_input(); outb(PS2_CMD, PS2_CMD_READ_CFG);
    uint8_t cfg = mouse_read();
    cfg |= 0x02;            /* enable the mouse interrupt */
    cfg &= (uint8_t)~0x20;  /* clear "mouse clock disabled" */
    wait_input(); outb(PS2_CMD, PS2_CMD_WRITE_CFG);
    wait_input(); outb(PS2_DATA, cfg);

    /* defaults, then start streaming - each command is ACKed */
    mouse_write(MOUSE_SET_DEFAULTS);
    if (mouse_read() != MOUSE_ACK) return 0;
    mouse_write(MOUSE_ENABLE_REPORT);
    if (mouse_read() != MOUSE_ACK) return 0;

    packet_idx = 0;
    irq_install_handler(12, mouse_callback);
    present = 1;
    return 1;
}

int      mouse_present(void)  { return present; }
int32_t  mouse_x(void)        { return mx; }
int32_t  mouse_y(void)        { return my; }
uint32_t mouse_buttons(void)  { return buttons; }
uint32_t mouse_seq(void)      { return moved; }

void mouse_set_bounds(int w, int h) {
    if (w > 0) clamp_w = w;
    if (h > 0) clamp_h = h;
    if (mx > clamp_w - 1) mx = clamp_w - 1;
    if (my > clamp_h - 1) my = clamp_h - 1;
}
