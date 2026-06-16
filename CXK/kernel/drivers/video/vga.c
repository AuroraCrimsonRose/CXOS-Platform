/* /CXLite/kernel/drivers/vga.c */
/* Aurora Tejeda */
/* Text-mode VGA renderer. Stateless cell drawing + hardware cursor. */

#include "vga.h"
#include "io.h"

#define VGA_MEM ((volatile uint16_t *)0xB8000)

/* CRT controller ports for the hardware cursor */
#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5

void vga_put_cell(int x, int y, char c, uint8_t attr) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return;
    VGA_MEM[y * VGA_WIDTH + x] = (uint16_t)(unsigned char)c | ((uint16_t)attr << 8);
}

void vga_clear(uint8_t attr) {
    uint16_t cell = (uint16_t)' ' | ((uint16_t)attr << 8);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEM[i] = cell;
}

void vga_set_cursor(int x, int y) {
    if (x < 0) x = 0;
    if (x >= VGA_WIDTH) x = VGA_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= VGA_HEIGHT) y = VGA_HEIGHT - 1;
    uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);

    outb(CRTC_INDEX, 0x0F);                 /* cursor location low  */
    outb(CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(CRTC_INDEX, 0x0E);                 /* cursor location high */
    outb(CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_disable_cursor(void) {
    outb(CRTC_INDEX, 0x0A);
    outb(CRTC_DATA, 0x20);                  /* bit 5 = cursor disable */
}

void vga_enable_cursor(void) {
    /* set cursor scanline start/end to make a visible block-ish cursor */
    outb(CRTC_INDEX, 0x0A);
    outb(CRTC_DATA, (inb(CRTC_DATA) & 0xC0) | 13);  /* start scanline */
    outb(CRTC_INDEX, 0x0B);
    outb(CRTC_DATA, (inb(CRTC_DATA) & 0xE0) | 15);  /* end scanline */
}