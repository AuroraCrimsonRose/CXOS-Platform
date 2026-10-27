// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/sysrec.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The one recording point (docs/kernel/CX_KERNEL_PLAN.md section 2.1,
 * docs/kernel/CX_SYSCALLS_IPC.md section 4).
 *
 * Every syscall passes sysrec_enter on the way in and sysrec_exit on the way
 * out, and IPC messages are syscalls, so they pass it too. Both are called
 * from syscall_dispatch (cpu/usermode.c) and from nowhere else. Nothing is
 * recorded yet beyond counts and the last call: the rule exists so tracing,
 * record and replay, and per-syscall statistics can hang off one place later
 * instead of being threaded through every handler.
 *
 * A call that does not return has an entry and no exit: SYS_EXIT leaves
 * through return_to_kernel, and a thread killed while blocked in a call never
 * comes back. So entered - exited is the calls that are in flight or ended
 * their process, not a leak.
 *
 * Whether every syscall really passes here is checked against two witnesses
 * kept outside this file: syscall_stub counts traps from ring 3, and the
 * dispatcher counts calls it handles (usermode.h). The self-test requires all
 * three to move together.
 */
#ifndef CXK_SYSREC_H
#define CXK_SYSREC_H

#include <stdint.h>
#include "systable.h"   /* SYSCALL_ARGS */

void sysrec_enter(uint32_t num, const uintptr_t args[SYSCALL_ARGS]);
void sysrec_exit(uint32_t num, int rc);

struct sysrec_stats {
    uint32_t entered;    /* calls that passed sysrec_enter */
    uint32_t exited;     /* calls that passed sysrec_exit */
    uint32_t last_num;   /* the most recent exit's syscall number */
    int32_t  last_rc;    /* and what it returned */
    uint32_t in_num;     /* the most recent entry's syscall number */
    uintptr_t in_args[SYSCALL_ARGS];  /* and its argument registers, whole */
};
void sysrec_snapshot(struct sysrec_stats *out);

#endif
