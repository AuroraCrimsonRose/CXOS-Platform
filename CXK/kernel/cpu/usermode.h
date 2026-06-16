/* /CXK/kernel/cpu/usermode.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * Ring 3 entry + syscall interface - Stage 1 (the privilege-boundary proof).
 *
 * usermode_init() installs the syscall gate (int 0x80, DPL=3 so ring 3 may
 * call it). usermode_test() drops into ring 3, runs a tiny user routine that
 * makes syscalls (write a string, then exit), and returns to the kernel - the
 * milestone being "we entered ring 3, ran code, syscalled, and came back".
 */

#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>

/* syscall numbers */
#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_GETPID  2    /* returns the current process id (0 for now - no
                            process model yet; placeholder for Stage 2) */

/* install the syscall IDT gate (int 0x80, DPL=3). call once at boot. */
void usermode_init(void);

/* run the ring-3 demo: enter user mode, run the test routine, return here.
   returns 0 on a clean round-trip. */
int usermode_test(void);

#endif