// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/early_con.c */
/* Aurora Tejeda / CATX Systems */
/* COM1 at 115200 8N1, polled - the settings of kernel/drivers/char/serial.c,
   so one terminal reads both kernels. See early_con.h. */

#include "early_con.h"
#include "arch/x86/pio.h"

#define COM1        0x3F8
#define LSR_THRE    0x20

void early_con_init(void) {
    arch_pio_out8(COM1 + 1, 0x00);      /* no interrupts */
    arch_pio_out8(COM1 + 3, 0x80);      /* divisor latch */
    arch_pio_out8(COM1 + 0, 1);         /* divisor 1: 115200 */
    arch_pio_out8(COM1 + 1, 0x00);
    arch_pio_out8(COM1 + 3, 0x03);      /* 8N1 */
    arch_pio_out8(COM1 + 2, 0xC7);      /* FIFO on, cleared */
    arch_pio_out8(COM1 + 4, 0x03);      /* DTR, RTS */
}

/* With no UART fitted the status port reads 0xFF, so this never spins. */
static void com1_put(char c) {
    while (!(arch_pio_in8(COM1 + 5) & LSR_THRE)) { }
    arch_pio_out8(COM1, (uint8_t)c);
}

void early_putc(char c) {
    if (c == '\n') com1_put('\r');
    com1_put(c);
}

void early_puts(const char *s) {
    while (*s) early_putc(*s++);
}

void early_hex(uint64_t v) {
    static const char digits[] = "0123456789ABCDEF";
    early_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        early_putc(digits[(v >> shift) & 0xF]);
}

void early_dec(uint64_t v) {
    char buf[21];
    int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) early_putc(buf[--i]);
}

void early_log_start(const char *tag, const char *sev) {
    int n = 0;
    early_putc('[');
    while (tag[n]) early_putc(tag[n++]);
    for (; n < 8; n++) early_putc(' ');
    early_puts("] - [");
    early_puts(sev);
    early_puts("] - ");
}

void early_log(const char *tag, const char *sev, const char *msg) {
    early_log_start(tag, sev);
    early_puts(msg);
    early_putc('\n');
}
