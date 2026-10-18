// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_iowin.c */
/* Aurora Tejeda / CATX Systems */
/* I/O windows (drivers/iowin.h): the right port or address, at the right width,
   and nothing outside the window. The port backend here records each access
   instead of executing it. */
#include "khost.h"
#include "drivers/iowin.h"

#include <string.h>

static struct { int count; uint16_t port; int width; int out; uint32_t value; } last;

static uint32_t fake_in(uint16_t port, int width) {
    last.count++; last.port = port; last.width = width; last.out = 0;
    return 0xA5A5A5A5u ^ port;      /* depends on the port, so a wrong one shows */
}
static void fake_out(uint16_t port, int width, uint32_t v) {
    last.count++; last.port = port; last.width = width; last.out = 1; last.value = v;
}
uint8_t  arch_pio_in8 (uint16_t p) { return (uint8_t)fake_in(p, 8); }
uint16_t arch_pio_in16(uint16_t p) { return (uint16_t)fake_in(p, 16); }
uint32_t arch_pio_in32(uint16_t p) { return fake_in(p, 32); }
void arch_pio_out8 (uint16_t p, uint8_t  v) { fake_out(p, 8, v); }
void arch_pio_out16(uint16_t p, uint16_t v) { fake_out(p, 16, v); }
void arch_pio_out32(uint16_t p, uint32_t v) { fake_out(p, 32, v); }

/* Each width reaches base + offset, with the instruction of that width. */
KH_TEST(iowin_port_reaches_base_plus_offset) {
    struct iowin w;
    KH_CHECK_EQ(iowin_port(&w, 0x1F0, 8), 0);
    memset(&last, 0, sizeof last);
    KH_CHECK_EQ(iowin_read8(&w, 7), (uint8_t)(0xA5A5A5A5u ^ 0x1F7));
    KH_CHECK_EQ(last.port, 0x1F7); KH_CHECK_EQ(last.width, 8);
    KH_CHECK_EQ(iowin_read16(&w, 0), (uint16_t)(0xA5A5A5A5u ^ 0x1F0));
    KH_CHECK_EQ(last.width, 16);
    KH_CHECK_EQ(iowin_read32(&w, 4), 0xA5A5A5A5u ^ 0x1F4);
    KH_CHECK_EQ(last.width, 32);
    iowin_write8(&w, 2, 0x5A);
    KH_CHECK_EQ(last.port, 0x1F2); KH_CHECK_EQ(last.width, 8);
    KH_CHECK_EQ(last.out, 1); KH_CHECK_EQ(last.value, 0x5A);
    iowin_write16(&w, 6, 0xBEEF);
    KH_CHECK_EQ(last.port, 0x1F6); KH_CHECK_EQ(last.width, 16); KH_CHECK_EQ(last.value, 0xBEEF);
    iowin_write32(&w, 0, 0xCAFEF00Du);
    KH_CHECK_EQ(last.port, 0x1F0); KH_CHECK_EQ(last.width, 32); KH_CHECK_EQ(last.value, 0xCAFEF00Du);
    KH_CHECK_EQ(last.count, 6);
}

/* Outside the window nothing reaches the bus: reads are all ones, writes are
   dropped, and both are counted. That includes an access that starts inside
   and runs past the end, and offsets big enough to wrap. */
KH_TEST(iowin_port_refuses_outside) {
    struct iowin w;
    KH_CHECK_EQ(iowin_port(&w, 0x3F8, 8), 0);
    memset(&last, 0, sizeof last);
    uint32_t before = iowin_refusals;
    KH_CHECK_EQ(iowin_read8(&w, 8), 0xFF);
    KH_CHECK_EQ(iowin_read16(&w, 7), 0xFFFF);
    KH_CHECK_EQ(iowin_read32(&w, 5), 0xFFFFFFFFu);
    KH_CHECK_EQ(iowin_read32(&w, 0xFFFFFFFEu), 0xFFFFFFFFu);
    iowin_write8(&w, 0x10000, 1);
    iowin_write32(&w, 0xFFFFFFFDu, 1);
    KH_CHECK_EQ(last.count, 0);
    KH_CHECK_EQ(iowin_refusals - before, 6);
    /* the last byte and the last dword that do fit are allowed */
    iowin_read8(&w, 7);
    iowin_read32(&w, 4);
    KH_CHECK_EQ(last.count, 2);
    KH_CHECK_EQ(iowin_refusals - before, 6);
}

/* A range that is empty or runs past port 0xFFFF opens nothing, and a window
   that failed to open refuses everything. */
KH_TEST(iowin_port_open_checks_range) {
    struct iowin w;
    KH_CHECK_EQ(iowin_port(&w, 0x80, 0), -1);
    KH_CHECK_EQ(iowin_port(&w, 0xFFFF, 2), -1);
    KH_CHECK_EQ(iowin_port(&w, 0x10000, 1), -1);
    KH_CHECK_EQ(iowin_port(&w, 0xFFFFFFFFu, 2), -1);
    memset(&last, 0, sizeof last);
    KH_CHECK_EQ(iowin_read8(&w, 0), 0xFF);
    KH_CHECK_EQ(last.count, 0);
    KH_CHECK_EQ(iowin_port(&w, 0xFFFF, 1), 0);
    KH_CHECK_EQ(iowin_port(&w, 0, 0x10000), 0);
}

/* Memory-mapped: the same offsets reach memory, at the width asked, and never
   the port backend. */
KH_TEST(iowin_mmio_reaches_memory) {
    static uint8_t regs[16];
    struct iowin w;
    memset(regs, 0, sizeof regs);
    KH_CHECK_EQ(iowin_mmio(&w, regs, 12), 0);
    memset(&last, 0, sizeof last);
    iowin_write32(&w, 8, 0x11223344u);
    iowin_write16(&w, 2, 0x5566);
    iowin_write8(&w, 0, 0x77);
    KH_CHECK_EQ(regs[8], 0x44); KH_CHECK_EQ(regs[11], 0x11);
    KH_CHECK_EQ(regs[2], 0x66); KH_CHECK_EQ(regs[3], 0x55);
    KH_CHECK_EQ(regs[0], 0x77); KH_CHECK_EQ(regs[1], 0);
    KH_CHECK_EQ(iowin_read32(&w, 8), 0x11223344u);
    KH_CHECK_EQ(iowin_read16(&w, 2), 0x5566);
    KH_CHECK_EQ(iowin_read8(&w, 0), 0x77);
    KH_CHECK_EQ(last.count, 0);
}

/* ...and nothing past the window's end, even though the memory is there. */
KH_TEST(iowin_mmio_refuses_outside) {
    static uint8_t regs[16];
    struct iowin w;
    memset(regs, 0, sizeof regs);
    KH_CHECK_EQ(iowin_mmio(&w, regs, 12), 0);
    uint32_t before = iowin_refusals;
    iowin_write32(&w, 12, 0xFFFFFFFFu);
    iowin_write32(&w, 10, 0xFFFFFFFFu);
    iowin_write8(&w, 0xFFFFFFFFu, 0xFF);
    KH_CHECK_EQ(iowin_read32(&w, 9), 0xFFFFFFFFu);
    for (int i = 0; i < 16; i++) KH_CHECK_EQ(regs[i], 0);
    KH_CHECK_EQ(iowin_refusals - before, 4);
    KH_CHECK_EQ(iowin_mmio(&w, regs, 0), -1);
    KH_CHECK_EQ(iowin_mmio(&w, (void *)(uintptr_t)-4, 8), -1);
}

/* A window fixed at build time behaves as one opened at run time. */
KH_TEST(iowin_static_window) {
    static struct iowin w = IOWIN_PORTS(0x70, 2);
    memset(&last, 0, sizeof last);
    iowin_write8(&w, 1, 0x42);
    KH_CHECK_EQ(last.port, 0x71); KH_CHECK_EQ(last.value, 0x42);
    uint32_t before = iowin_refusals;
    iowin_write8(&w, 2, 0x42);
    KH_CHECK_EQ(last.count, 1);
    KH_CHECK_EQ(iowin_refusals - before, 1);
}
