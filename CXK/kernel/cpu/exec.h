/* /CXK/kernel/cpu/exec.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * cxex_exec: the executive launcher - the kernel-to-ring-3 handoff for a signed
 * CXEX image. Verifies the image against the embedded trusted key, creates a
 * fresh address space, loads the image's sections into it, sets up a user
 * stack, and drops to ring 3 at the image's entry point. Returns when the image
 * SYS_EXITs.
 *
 * This is the BLOCKING form (runs on the calling kernel thread, start to exit).
 * Concurrent scheduler-managed processes - each in its own space with CR3
 * switched on context switch - are a separate step on top of this.
 */

#ifndef EXEC_H
#define EXEC_H

#include <stdint.h>
#include <stddef.h>

/* Verify, load, and run a CXEX image in ring 3 until it exits.
   Returns the image's exit code (>=0), or a negative cxex_exec error. */
int cxex_exec(const uint8_t *file, size_t len);

enum cxex_exec_result {
    CXEX_EXEC_VERIFY_FAILED = -1,   /* signature/identity check failed */
    CXEX_EXEC_BAD_TYPE      = -2,   /* not an OS/USER executable */
    CXEX_EXEC_NOSPACE       = -3,   /* address space creation failed */
    CXEX_EXEC_LOAD_FAILED   = -4,   /* cxex_load failed */
    CXEX_EXEC_NOMEM         = -5    /* user stack allocation failed */
};

#endif