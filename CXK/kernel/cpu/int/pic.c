/* /CXLite/kernel/cpu/pic.c */
/* Aurora Tejeda */
/* Remap the 8259 PIC so hardware IRQs land on vectors 32..47,
   clear of the CPU exception vectors 0..31. */

#include <stdint.h>
#include "io.h"
#include "pic.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20

void pic_remap(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();

    outb(PIC1_DATA, 0x20); io_wait();   /* master vectors 0x20.. */
    outb(PIC2_DATA, 0x28); io_wait();   /* slave  vectors 0x28.. */

    outb(PIC1_DATA, 0x04); io_wait();   /* tell master: slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();   /* tell slave: cascade identity */

    outb(PIC1_DATA, 0x01); io_wait();   /* 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint32_t int_no) {
    if (int_no >= 40) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}