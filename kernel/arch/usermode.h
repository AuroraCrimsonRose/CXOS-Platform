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

/* The stub for syscall `nr` in the system page (cpu/syspage.c): at most
   SYSPAGE_STUB_SIZE bytes at `dst` that put nr in eax, enter the kernel
   through this architecture's gate and return to the caller. */
void arch_syscall_stub(uint8_t *dst, uint32_t nr);

/* The offset in a stub of the instruction after its trap: where a call made
   through the stub returns to ring 3, and so the only return address the
   gate accepts for that number (syscall_check_origin). */
uint32_t arch_syscall_stub_ret(void);

/* Run user code at `entry` on the user stack `user_sp`, with every register
   zero but the one that carries `sysbase`, the system page's base (esi on
   x86-32, rsi on x86-64; cxk_abi.h). The kernel's own return state is kept
   in save_slot (the thread's, thread_current_usave()). Returns only when that
   code ends, with the value given to return_to_kernel. */
int  enter_usermode(uintptr_t entry, uintptr_t user_sp, uintptr_t *save_slot,
                    uintptr_t sysbase);

/* From inside a system call or a user fault: abandon the user code and return
   `retval` from the enter_usermode that started it. */
void return_to_kernel(int retval, uintptr_t *save_slot);

#endif
