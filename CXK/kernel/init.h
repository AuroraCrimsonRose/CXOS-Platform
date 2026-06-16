/* /CXLite/kernel/init.h */
/* Aurora Tejeda */
/*
 * System initialization. kernel.c brings up the console (so we can show output)
 * then calls system_init(), which initializes every subsystem, tracks pass/fail
 * for each, and reports:
 *   - on full success: a single consolidated line listing each subsystem with a
 *     pass mark, e.g. "[KERNEL] SUBSYSTEMS: IDT ^ TIMER ^ ATA ^ USB ^ ..."
 *   - on any failure: a full, explicit per-failure line, e.g. "[KERNEL] USB INIT FAIL"
 *
 * This keeps kernel.c lean - it just gets us to the console, runs init, and
 * launches the shell (the final destination).
 */

#ifndef INIT_H
#define INIT_H

/* Initialize all subsystems (everything after console/fpu, which kernel.c does
   first so init output is visible). Prints the consolidated status report.
   Returns the number of subsystems that FAILED (0 = clean boot). Interrupts
   are still disabled on return; kernel.c does sti afterward. */
int system_init(void);

#endif