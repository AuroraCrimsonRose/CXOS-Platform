// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/drivers/char/serial.c */
/* Aurora Tejeda */
/* 16550 UART on COM1. See serial.h for what this is for. */

#include "serial.h"
#include "io.h"

#if CXK_ENABLE_SERIAL

#define COM1 0x3F8

/* Register offsets. 0 and 1 change meaning when DLAB (LCR bit 7) is set: they
   become the low and high halves of the baud divisor. */
#define REG_DATA        0   /* THR write / RBR read, or DLL when DLAB */
#define REG_IER         1   /* interrupt enable,     or DLH when DLAB */
#define REG_FCR         2   /* FIFO control (write)                   */
#define REG_LCR         3   /* line control                           */
#define REG_MCR         4   /* modem control                          */
#define REG_LSR         5   /* line status                            */

#define LCR_8N1         0x03    /* 8 data bits, no parity, 1 stop bit */
#define LCR_DLAB        0x80    /* divisor latch access               */

#define FCR_ENABLE      0x01
#define FCR_CLEAR_RX    0x02
#define FCR_CLEAR_TX    0x04
#define FCR_TRIGGER_14  0xC0

#define MCR_DTR         0x01
#define MCR_RTS         0x02
#define MCR_OUT2        0x08    /* gates the UART's IRQ onto the PIC  */
#define MCR_LOOPBACK    0x10

#define LSR_DR          0x01    /* data ready (a byte is waiting in RBR)  */
#define LSR_THRE        0x20    /* transmitter holding register empty */

/* 115200 baud: the UART's clock is 1.8432 MHz and it samples 16x, so the
   divisor is 1843200 / (16 * baud). Highest standard rate, and the one QEMU
   and every terminal default to. */
#define BAUD_DIVISOR    1

/* How long to wait for the transmitter before giving up on a byte. The point
   is that this terminates: if a byte is dropped the log loses a character,
   whereas an unbounded spin on hardware that never raises THRE hangs the boot
   and looks exactly like a kernel fault. Haiku's 8250 driver bounds its wait
   for the same reason; Serenity's does not, but its driver only runs against a
   port it has already enumerated. */
#define TX_SPIN_LIMIT   100000

/* How long the loopback probe waits for its byte to come back round. Short:
   this runs twice on a path where nothing has been printed yet, and a machine
   with no UART must not spend real time here. */
#define LOOPBACK_WAIT   1000

static int active = 0;

void serial_init(void)
{
    active = 0;

    outb(COM1 + REG_IER, 0x00);                 /* no interrupts: polled only */

    outb(COM1 + REG_LCR, LCR_DLAB);             /* divisor latch visible      */
    outb(COM1 + REG_DATA, BAUD_DIVISOR & 0xFF);
    outb(COM1 + REG_IER, (BAUD_DIVISOR >> 8) & 0xFF);
    outb(COM1 + REG_LCR, LCR_8N1);              /* latch away, 8N1            */

    outb(COM1 + REG_FCR, FCR_ENABLE | FCR_CLEAR_RX | FCR_CLEAR_TX | FCR_TRIGGER_14);

    /* Is anything actually there? Put the UART in loopback and require two
       bytes back. Without this, a machine with no COM1 reads 0xFF from every
       register, LSR_THRE never clears, and each putc burns the full spin limit
       - which turns a debug aid into a boot that crawls.

       Two details, both taken from Redox's uart_16550 and both load-bearing:
       wait for DR before reading, because the byte needs time to come round
       and reading immediately can fail the probe on hardware that is perfectly
       fine; and use the alternating patterns 0x55 / 0xAA, so a bus stuck at a
       value cannot pass by luck. */
    outb(COM1 + REG_MCR, MCR_LOOPBACK | MCR_OUT2 | MCR_RTS | MCR_DTR);
    {
        static const uint8_t pattern[2] = { 0x55, 0xAA };
        int i;

        for (i = 0; i < 2; i++) {
            int waits = LOOPBACK_WAIT;

            outb(COM1 + REG_DATA, pattern[i]);
            while ((inb(COM1 + REG_LSR) & LSR_DR) == 0) {
                if (--waits == 0) break;
            }
            if (inb(COM1 + REG_DATA) != pattern[i]) {
                outb(COM1 + REG_MCR, 0x00);
                return;                         /* no UART: stay inactive */
            }
        }
    }

    outb(COM1 + REG_MCR, MCR_OUT2 | MCR_RTS | MCR_DTR);
    active = 1;
}

int serial_active(void)
{
    return active;
}

void serial_putc(char c)
{
    int spins = TX_SPIN_LIMIT;

    if (!active) return;

    while ((inb(COM1 + REG_LSR) & LSR_THRE) == 0) {
        if (--spins == 0) return;               /* drop the byte, keep booting */
    }
    outb(COM1 + REG_DATA, (uint8_t)c);
}

void serial_puts(const char *s)
{
    if (!active) return;
    while (*s) serial_putc(*s++);
}

#else  /* !CXK_ENABLE_SERIAL */

void serial_init(void)            { }
int  serial_active(void)          { return 0; }
void serial_putc(char c)          { (void)c; }
void serial_puts(const char *s)   { (void)s; }

#endif
