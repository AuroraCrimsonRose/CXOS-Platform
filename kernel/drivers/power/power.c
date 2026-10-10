// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/power/power.c */
/* Aurora Tejeda */
/* Power management: ACPI shutdown (S5), reboot (ACPI reset / 8042), and a
   low-power sleep (ACPI S1 if supported, else C1 halt-idle). */

#include "power.h"
#include "io.h"
#include "console.h"
#include "vga.h"
#include "color.h"   /* v5: vga_attr(fg,bg) packs the attribute byte */
#include "acpi.h"
#include "timer.h"
#include "keyboard.h"

/* Reboot: prefer the ACPI reset register (works on the widest range of real
   hardware), then the 8042 keyboard-controller pulse, then a triple fault. */
void power_reboot(void) {
    __asm__ volatile ("cli");

    /* 1. ACPI reset register, if the FADT provided one */
    acpi_reboot();   /* returns only if it didn't work */

    /* 2. 8042 keyboard controller: pulse the CPU reset line */
    uint8_t status;
    do {
        status = inb(0x64);
        if (status & 0x01) inb(0x60);   /* drain output buffer */
    } while (status & 0x02);            /* wait while input buffer full */
    outb(0x64, 0xFE);                   /* pulse: CPU reset */

    /* 3. triple fault via a zero-limit IDT */
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile ("lidt %0; int $0x03" : : "m"(null_idt));

    for (;;) __asm__ volatile ("hlt");
}

void power_shutdown(void) {
    /* real ACPI S5 poweroff if available */
    if (acpi_can_shutdown()) {
        console_set_color(vga_attr(VGA_YELLOW, VGA_BLACK));
        console_puts("\nShutting down...\n");
        console_set_color(vga_attr(VGA_LIGHT_GREY, VGA_BLACK));
        acpi_shutdown();    /* does not return on success */
    }

    /* emulator fallback: QEMU/Bochs ACPI poweroff ports (no effect on real HW) */
    outw(0x604,  0x2000);   /* QEMU */
    outw(0xB004, 0x2000);   /* Bochs / older QEMU */

    /* last resort: halt with the classic message */
    console_set_color(vga_attr(VGA_YELLOW, VGA_BLACK));
    console_puts("\nIt is now safe to turn off your computer.\n");
    console_set_color(vga_attr(VGA_LIGHT_GREY, VGA_BLACK));
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* C1 idle: halt the CPU until an interrupt. If ms > 0, idle for ~ms
   milliseconds (waking early on a keypress); if ms == 0, idle until a key. */
static void c1_idle(uint32_t ms) {
    uint32_t start = timer_ticks();    /* 1 tick = 1 ms */
    for (;;) {
        char c = keyboard_getchar();
        if (c != 0) break;             /* any key wakes/cancels */
        if (ms > 0 && (timer_ticks() - start) >= ms) break;
        __asm__ volatile ("hlt");      /* sleep until next interrupt (>=1ms via PIT) */
    }
}

/* power_sleep: low-power idle via C1 (CPU halt). With no duration (ms==0) it
   idles until a key is pressed; with a duration it idles for ~ms (waking early
   on a key).

   NOTE: I use C1 rather than ACPI S1 even where S1 is supported. S1 *entry*
   works on this hardware, but waking from S1 needs a properly-armed wake source
   (the keyboard's GPE, discoverable only by parsing the DSDT's _PRW objects -
   an AML-interpreter job for the future; the power button alone didn't wake it
   on the test board). C1 idle wakes reliably on any interrupt, so it's the
   safe, dependable low-power state. S1 is still detected and reported at boot;
   we just don't enter it until keyboard wake can be set up properly. */
void power_sleep(uint32_t ms) {
    if (ms == 0)
        console_puts("Idling (CPU low-power, press a key to wake)...\n");
    c1_idle(ms);
}