// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/pic.c */
/* Aurora Tejeda */
/* Remap the 8259 PIC so hardware IRQs land on vectors 32..47,
   clear of the CPU exception vectors 0..31. */

#include <stdint.h>
#include "drivers/iowin.h"
#include "pic.h"

/* the master and slave controllers: command at +0, data (the mask) at +1 */
static struct iowin pic1 = IOWIN_PORTS(0x20, 2);
static struct iowin pic2 = IOWIN_PORTS(0xA0, 2);
#define PIC_CMD    0
#define PIC_DATA   1
#define PIC_EOI    0x20

/* A write to the POST diagnostic port takes about a microsecond: long enough
   for an old PIC to take in the previous command. */
static struct iowin post = IOWIN_PORTS(0x80, 1);
static void io_wait(void) { iowin_write8(&post, 0, 0); }

void pic_remap(void) {
    uint8_t mask1 = iowin_read8(&pic1, PIC_DATA);
    uint8_t mask2 = iowin_read8(&pic2, PIC_DATA);

    iowin_write8(&pic1, PIC_CMD, 0x11); io_wait();
    iowin_write8(&pic2, PIC_CMD, 0x11); io_wait();

    iowin_write8(&pic1, PIC_DATA, 0x20); io_wait();   /* master vectors 0x20.. */
    iowin_write8(&pic2, PIC_DATA, 0x28); io_wait();   /* slave  vectors 0x28.. */

    iowin_write8(&pic1, PIC_DATA, 0x04); io_wait();   /* tell master: slave on IRQ2 */
    iowin_write8(&pic2, PIC_DATA, 0x02); io_wait();   /* tell slave: cascade identity */

    iowin_write8(&pic1, PIC_DATA, 0x01); io_wait();   /* 8086 mode */
    iowin_write8(&pic2, PIC_DATA, 0x01); io_wait();

    iowin_write8(&pic1, PIC_DATA, mask1);
    iowin_write8(&pic2, PIC_DATA, mask2);
}

void pic_send_eoi(uint32_t int_no) {
    if (int_no >= 40) iowin_write8(&pic2, PIC_CMD, PIC_EOI);
    iowin_write8(&pic1, PIC_CMD, PIC_EOI);
}

/* Clear one line's mask bit. For a slave line (8..15) the master's cascade
   input, IRQ2, must be unmasked too: the slave reaches the CPU only through it,
   so leaving the cascade masked silently swallows every slave interrupt. */
void pic_unmask(int irq) {
    if (irq < 0 || irq >= 16) return;

    if (irq < 8) {
        iowin_write8(&pic1, PIC_DATA, (uint8_t)(iowin_read8(&pic1, PIC_DATA) & ~(1u << irq)));
    } else {
        iowin_write8(&pic2, PIC_DATA, (uint8_t)(iowin_read8(&pic2, PIC_DATA) & ~(1u << (irq - 8))));
        iowin_write8(&pic1, PIC_DATA, (uint8_t)(iowin_read8(&pic1, PIC_DATA) & ~(1u << 2)));   /* cascade */
    }
}

void pic_mask(int irq) {
    if (irq < 0 || irq >= 16) return;

    if (irq < 8) {
        iowin_write8(&pic1, PIC_DATA, (uint8_t)(iowin_read8(&pic1, PIC_DATA) | (1u << irq)));
    } else {
        iowin_write8(&pic2, PIC_DATA, (uint8_t)(iowin_read8(&pic2, PIC_DATA) | (1u << (irq - 8))));
    }
}

/* Mask every line on both chips. Used when the I/O APIC takes over: the two
   controllers are wired to the same interrupt lines, so leaving the 8259 live
   means each interrupt is delivered twice - once acknowledged through the local
   APIC and once left pending on the PIC, which then wedges that line. */
void pic_mask_all(void) {
    iowin_write8(&pic1, PIC_DATA, 0xFF);
    iowin_write8(&pic2, PIC_DATA, 0xFF);
}

/* Current mask state of both chips as one 16-bit word, bit n set = IRQ n
   masked. Read before handing over to the I/O APIC so the same lines stay
   enabled and no others. */
uint16_t pic_get_masks(void) {
    return (uint16_t)(iowin_read8(&pic1, PIC_DATA) | ((uint16_t)iowin_read8(&pic2, PIC_DATA) << 8));
}
