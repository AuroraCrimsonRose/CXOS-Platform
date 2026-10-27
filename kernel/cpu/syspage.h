// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/syspage.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The system page (docs/kernel/CX_SYSCALLS_IPC.md §8): one read-only page of
 * code in every process, a stub per syscall SYSPAGE_STUB_SIZE bytes apart
 * (cxk_abi.h). A program is handed its base at entry and calls through it;
 * it never names the address, so the kernel can move it.
 *
 * Where it goes, at the top of the user half (x86-32 addresses on both
 * kernels until user addresses widen):
 *
 *   0xBFFFF000  the argument page (USER_ARGS_BASE)
 *   0xBFFBF000  the user stack, 64 pages, growing down (spawn.c)
 *   0xBFFBE000  unmapped: the stack's guard
 *   0xBFFBD000  the system page
 *   0xBFFBC000  unmapped
 *   0xBFF00000  USER_IMAGE_END: no image section reaches past it
 *
 * The loader refuses a section above USER_IMAGE_END, so an image can never
 * be mapped over the stack, the arguments or the stubs.
 */

#ifndef SYSPAGE_H
#define SYSPAGE_H

#include "cxk_abi.h"
#include "arch/paging.h"
#include <stdint.h>

#define SYSPAGE_VIRT     0xBFFBD000u
#define USER_IMAGE_END   0xBFF00000u

/* Write the stubs into one page: the stub for each syscall the dispatch
   table has, int3 everywhere else. */
void syspage_fill(uint8_t *page);

/* Map a freshly filled system page at `at` in the current address space,
   readable and executable from ring 3, never writable. The frame is the
   caller's to free, through phys_out (NULL in a process: the teardown
   frees it with the rest).
   0, or -1 with nothing mapped. */
int syspage_map(uintptr_t at, paddr_t *phys_out);

#endif
