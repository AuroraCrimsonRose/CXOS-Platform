// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/usercopy.c */
/* Aurora Tejeda / CATX Systems */
/* Checking and copying user buffers (usermode.h). Apart from usermode.c, which
   brings the whole syscall table with it, so that a kernel without the
   drivers - the x86-64 one until step 5f - checks ring 3's pointers with the
   same code. */

#include "usermode.h"
#include "arch/paging.h"

/* ---- syscall pointer validation -------------------------------------------
 * With a privilege boundary in place, the kernel must not blindly dereference
 * pointers handed up from ring 3. Validate against the current process's user
 * region. */

/* Shared body. `need_write` decides which question is actually being asked of
   the page tables - see user_ptr_readable / user_ptr_writable below. */
static int user_ptr_span(uint32_t ptr, uint32_t len, int need_write) {
    if (len == 0) len = 1;
    if (ptr + len < ptr)            return 0;   /* wrap/overflow */
    if (ptr + len > USER_ADDR32_END) return 0;  /* must lie entirely in the user half */
    /* every page the buffer spans must be present + ring-3 accessible in the
       caller's active address space (its CR3 is live during this syscall). */
    for (uint32_t p = ptr & ~0xFFFu; p < ptr + len; p += 0x1000) {
        if (need_write ? !paging_is_user_writable(p) : !paging_is_user(p)) return 0;
    }
    return 1;
}

/* A buffer the kernel will only READ from. */
int user_ptr_readable(uint32_t ptr, uint32_t len) { return user_ptr_span(ptr, len, 0); }

/* A buffer the kernel will WRITE to.
 *
 * There was one check for both, and it tested presence and the user bit only
 * (security review §4). A read-only user page passed it, and the kernel's write
 * then went through regardless - ring 0 ignores the read-only bit unless CR0.WP
 * is set. So a process could hand a syscall a pointer into its own text and have
 * the kernel scribble on it, which is both a way to defeat W^X from the other
 * side and a way to corrupt a page the process had every reason to think was
 * immutable.
 *
 * The single function is deliberately gone rather than kept as an alias: every
 * call site has to say which access it means, and a new one cannot default to
 * the weaker check by forgetting.
 */
int user_ptr_writable(uint32_t ptr, uint32_t len) { return user_ptr_span(ptr, len, 1); }

/* ---- copying across the ring boundary (security review §6) ---------------
 *
 * The review's case is a syscall that validates a user pointer, blocks, and
 * then copies through the pointer it validated before sleeping. IPC did
 * exactly that on both of its blocking paths. The checks above are passive
 * reads of the page tables, so such a check is a snapshot of the mapping at
 * one instant and nothing refreshes it.
 *
 * So the copy carries its own validation, and the two layers are *both*
 * load-bearing:
 *
 *   the range check   decides POLICY: is this address in the user half, and
 *                     may ring 3 write there. It also refuses before a single
 *                     byte moves.
 *   the recoverable   is the hardware BACKSTOP: a page that is not there, or
 *   copy              stopped being there after the check. Validation cannot
 *                     be made to hold across the instruction that uses it, so
 *                     the copy is allowed to fail instead (usermode.asm).
 *
 * CR0.WP (set in arch/x86_32/entry.asm) now makes the CPU enforce the read-only bit
 * against ring 0 too, so the two overlap on one case - a read-only user page -
 * where before WP only the check caught it. They are still not the same check,
 * and the range check is not redundant:
 *
 *   - WP enforces the WRITE bit. It does not enforce the USER bit, and nothing
 *     stops ring 0 touching a kernel address. A copy-out to a kernel address,
 *     or to a page writable but not ring-3-accessible, passes the hardware and
 *     is caught only here.
 *   - A fault stops the copy WHERE IT FAULTS. For a buffer spanning several
 *     pages, writable at the start and read-only later, recovery alone leaves
 *     the first pages modified. The check refuses the whole thing up front.
 *
 * ktest proves them separately, and has to: once WP makes both refuse a
 * read-only page, the return value no longer says which one did it, so the
 * tests assert on usercopy_faults_recovered() - refused by the check means no
 * fault was taken at all.
 *
 * This is the shape every system that has solved it converged on: Haiku's
 * user_memcpy validates the range and traps the fault, returning
 * B_BAD_ADDRESS; Mach copies through copyoutmsg, whose copy_validate runs per
 * copy against the target map, with asm recovery entries under it; Redox does
 * not pre-check the mapping at all and relies entirely on a recoverable copy
 * returning EFAULT. None of them hold a probe across a block.
 */
/* Recovered-fault accounting. The counter lives here, with the rest of the
   user-copy bookkeeping, and the fault handler only reports the event: idt.c
   knows about faults, this file knows what a user copy is. */
static uint32_t recovered_faults = 0;

uint32_t usercopy_faults_recovered(void)  { return recovered_faults; }
void     usercopy_note_recovered_fault(void) { recovered_faults++; }

int user_copy_out(uint32_t udst, const void *ksrc, uint32_t len) {
    if (len == 0) return 0;
    if (!user_ptr_writable(udst, len)) return E_FAULT;
    if (user_copy_bytes(user_ptr(udst), ksrc, len) != 0) return E_FAULT;
    return 0;
}

int user_copy_in(void *kdst, uint32_t usrc, uint32_t len) {
    if (len == 0) return 0;
    if (!user_ptr_readable(usrc, len)) return E_FAULT;
    if (user_copy_bytes(kdst, user_ptr(usrc), len) != 0) return E_FAULT;
    return 0;
}
