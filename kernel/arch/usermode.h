// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/usermode.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Entering and leaving user mode, as generic code sees it. Implemented in
 * kernel/arch/<arch>/ (on 32-bit x86, usermode.asm and the int 0x80 gate).
 */

#ifndef ARCH_USERMODE_H
#define ARCH_USERMODE_H

#include <stdint.h>

/* Install the gate user code calls the kernel through. Each call arrives at
   syscall_dispatch (cpu/usermode.h). */
void arch_syscall_gate_init(void);

/* Run user code at `entry` on the user stack `user_sp`. The kernel's own
   return state is kept in save_slot (the thread's, thread_current_usave()).
   Returns only when that code ends, with the value given to return_to_kernel. */
int  enter_usermode(uintptr_t entry, uintptr_t user_sp, uintptr_t *save_slot);

/* From inside a system call or a user fault: abandon the user code and return
   `retval` from the enter_usermode that started it. */
void return_to_kernel(int retval, uintptr_t *save_slot);

#endif
