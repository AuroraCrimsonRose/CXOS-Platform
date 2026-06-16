/* /CXLite/kernel/kernel.c */
/* Aurora Tejeda        */
/*
 * Kernel entry. Kept deliberately lean: bring up just enough to show output
 * (framebuffer + console + FPU), hand off all subsystem initialization to
 * system_init() in init.c, then land at the final destination - the shell.
 */

#include "console.h"
#include "vga.h"
#include "fb.h"
#include "fpu.h"
#include "shell.h"
#include "init.h"

void kmain(void)
{
    /* Framebuffer FIRST: if the bootloader set a VBE mode, bring up the
       framebuffer so the console can render to it. If not, fb_init() returns
       -1 and the console falls back to VGA text mode. */
    fb_init();

    /* Console next - picks the framebuffer backend if active, else VGA text.
       All boot output goes through it, so it must be up before system_init. */
    console_init();

    /* FPU + SSE before any floating-point code (e.g. the spin demo). */
    fpu_init();

    /* Bring up every subsystem and print the consolidated status report.
       Returns the number of failures (0 = clean boot). */
    int failures = system_init();

    /* interrupts on - from here the system is live */
    __asm__ volatile ("sti");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    if (failures == 0) {
        console_print("\nCXK ready. Type 'help'.\n\n");
    } else {
        console_print("\nCXK ready (with warnings). Type 'help'.\n\n");
    }

    /* final destination: the interactive shell (never returns) */
    shell_run();
}