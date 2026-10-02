/* /kernel/cpu/usermode.h */
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
#include "cxk_abi.h"

/* syscall numbers */

/* install the syscall IDT gate (int 0x80, DPL=3). call once at boot. */
void usermode_init(void);
/* Validate a ring-3 buffer. Ask for the access you intend: the writable form
   also requires the pages to be writable by ring 3, which the single old
   user_ptr_ok did not check (security review §4). */
int  user_ptr_readable(uint32_t ptr, uint32_t len);
int  user_ptr_writable(uint32_t ptr, uint32_t len);

/* SYS_FILE_OP exercised over a real PAGE_USER mapping (see usermode.c).
   1 = pass, 0 = fail. Lives here because it needs map_user_page. */
int  usermode_file_test(void);

/* register the ring-3 fault handler so user faults kill the process, not the
   kernel. call once at boot after the scheduler is up. */
void usermode_register_fault_handler(void);

/* run the ring-3 demo: enter user mode, run the test routine, return here.
   returns 0 on a clean round-trip. */
int usermode_test(void);

/* Create a ring-3 PROCESS managed by the scheduler: a scheduler thread whose
   kernel-stack trampoline drops into ring 3 to run the given user routine, and
   calls thread_exit() (reaped by the scheduler) when the routine SYS_EXITs.
   `blob`/`blob_len` is the position-independent user code to run; `msg` is an
   optional string placed in the process's user page (passed on its user stack).
   returns the new process id (pid), or -1. */
int process_create_ring3(const char *name,
                         const void *blob, uint32_t blob_len,
                         const char *msg);

/* Launch a ring-3 process owned by a specific human user (uid >= 1). Enforces
   the identity invariant: rejects uid == 0 (SYSTEM) - a user is never UID 0.
   Returns the new pid, or -1. */
int process_create_ring3_as_user(const char *name,
                                 const void *blob, uint32_t blob_len,
                                 const char *msg, uint32_t uid);

#endif