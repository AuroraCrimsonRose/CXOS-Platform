// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/platform/pc/vga.c */
/* Aurora Tejeda */
/* Text-mode VGA renderer. Stateless cell drawing + hardware cursor. */

#include "arch/paging.h"   /* KERNEL_VBASE */
#include "vga.h"
#include "drivers/iowin.h"

/* higher-half VGA text buffer: physical 0xB8000 mapped at 0xC0000000 + 0xB8000
   by the boot page tables. (v4 used physical 0xB8000, which is unmapped in v5
   once the identity map is dropped.) */
#define VGA_MEM ((volatile uint16_t *)(KERNEL_VBASE + 0xB8000))

/* CRT controller ports for the hardware cursor */
static struct iowin crtc = IOWIN_PORTS(0x3D4, 2);
#define CRTC_INDEX 0
#define CRTC_DATA  1

void vga_put_cell(int x, int y, char c, uint8_t attr) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return;
    VGA_MEM[y * VGA_WIDTH + x] = (uint16_t)(unsigned char)c | ((uint16_t)attr << 8);
}

void vga_clear(uint8_t attr) {
    uint16_t cell = (uint16_t)' ' | ((uint16_t)attr << 8);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEM[i] = cell;
}

void vga_scroll_up(uint8_t attr) {
    for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++)
        VGA_MEM[i] = VGA_MEM[i + VGA_WIDTH];
    for (int x = 0; x < VGA_WIDTH; x++)
        vga_put_cell(x, VGA_HEIGHT - 1, ' ', attr);
}

void vga_set_cursor(int x, int y) {
    if (x < 0) x = 0;
    if (x >= VGA_WIDTH) x = VGA_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= VGA_HEIGHT) y = VGA_HEIGHT - 1;
    uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);

    iowin_write8(&crtc, CRTC_INDEX, 0x0F);                 /* cursor location low  */
    iowin_write8(&crtc, CRTC_DATA, (uint8_t)(pos & 0xFF));
    iowin_write8(&crtc, CRTC_INDEX, 0x0E);                 /* cursor location high */
    iowin_write8(&crtc, CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_disable_cursor(void) {
    iowin_write8(&crtc, CRTC_INDEX, 0x0A);
    iowin_write8(&crtc, CRTC_DATA, 0x20);                  /* bit 5 = cursor disable */
}

void vga_enable_cursor(void) {
    /* set cursor scanline start/end to make a visible block-ish cursor */
    iowin_write8(&crtc, CRTC_INDEX, 0x0A);
    iowin_write8(&crtc, CRTC_DATA, (iowin_read8(&crtc, CRTC_DATA) & 0xC0) | 13);  /* start scanline */
    iowin_write8(&crtc, CRTC_INDEX, 0x0B);
    iowin_write8(&crtc, CRTC_DATA, (iowin_read8(&crtc, CRTC_DATA) & 0xE0) | 15);  /* end scanline */
}