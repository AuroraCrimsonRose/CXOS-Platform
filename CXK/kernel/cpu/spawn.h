/* /CXK/kernel/cpu/spawn.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * ABI v1 - spawn (see docs/CXK_ABI_v1 sec 7, syscall 0x70). The kernel copies
 * the app image out of the executive's space, creates a fresh address space,
 * creates a scheduler thread whose trampoline cxex_loads + drops to ring 3,
 * forces the child's caps to 0, and installs a SEND handle to the executive's
 * endpoint as the child's handle 0. struct spawn_args lives in the shared
 * public ABI header. Returns the new pid, or a negative ABI error.
 */

#ifndef SPAWN_H
#define SPAWN_H

#include "cxk_abi.h"   /* struct spawn_args + the public ABI contract */

/* syscall handler (called from the dispatcher after the CAP_SPAWN check).
   `ua` is a user pointer to a struct spawn_args in the caller's space. */
int sys_spawn(const struct spawn_args *ua);

#endif