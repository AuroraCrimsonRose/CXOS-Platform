/* /CXK/kernel/drivers/power/power.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Minimal reboot for the shell's `reboot` command. Ported from the v4 power
   driver, keeping the two dependency-free reset paths (8042 controller pulse
   and triple fault) and dropping the ACPI reset step - ACPI can be re-added
   later when acpi.c is ported from v4. Both paths work on QEMU/Bochs and real
   hardware. */

#include "power.h"
#include "../../cpu/io.h"

void power_reboot(void) {
    __asm__ volatile ("cli");

    /* 1. 8042 keyboard controller: pulse the CPU reset line.
       Drain the output buffer and wait for the input buffer to clear first. */
    uint8_t status;
    do {
        status = inb(0x64);
        if (status & 0x01) inb(0x60);   /* drain output buffer */
    } while (status & 0x02);            /* wait while input buffer full */
    outb(0x64, 0xFE);                   /* pulse: CPU reset */

    /* 2. triple fault via a zero-limit IDT (if the 8042 pulse didn't take) */
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile ("lidt %0; int $0x03" : : "m"(null_idt));

    for (;;) __asm__ volatile ("hlt");
}
