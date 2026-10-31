// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/usermode.h */
/* Aurora Tejeda / CATX Systems */
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

/* The syscall entry: runs one call through the recording point (sysrec.h)
   and the generated table (systable.h), with the five argument registers at
   the machine's width (a u64 takes two on x86-32, systable.h). syscall_stub
   calls it for ring 3; the self-tests call it directly. */
int syscall_dispatch(uint32_t num, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                     uintptr_t a4, uintptr_t a5);

/* The gate's first step, before the call is counted or dispatched: `ip` is
   where the trap returns to ring 3, which must be the return point of the
   system page's stub for `num` (CX_SYSCALLS_IPC.md §4). A call from anywhere
   else ends the process as a fault does, back out of enter_usermode with
   USERMODE_FAULTED; then this does not return. */
void syscall_check_origin(uint32_t num, uintptr_t ip);

/* Calls syscall_check_origin refused since boot, for the self-tests. */
uint32_t syscall_refused_count(void);

/* Witnesses for the recording point, counted apart from it: traps the gate
   let through (past syscall_check_origin), and calls the dispatcher handled. */
uint32_t syscall_trapped_count(void);
uint32_t syscall_dispatch_count(void);
/* Validate a ring-3 buffer. Ask for the access you intend: the writable form
   also requires the pages to be writable by ring 3, which the single old
   user_ptr_ok did not check (security review §4). */
int  user_ptr_readable(uint64_t ptr, uint64_t len);
int  user_ptr_writable(uint64_t ptr, uint64_t len);

/* A user address (the ABI carries them as uint64_t, never as C pointers) as a
   pointer the kernel may dereference: only after user_ptr_readable or
   user_ptr_writable said yes for the whole span. Prefer the copies below. */
static inline void *user_ptr(uint64_t uaddr) { return (void *)(uintptr_t)uaddr; }
/* The reverse, for the self-tests that build a call in a user-half buffer. */
static inline uint64_t user_addr(const void *p) { return (uint64_t)(uintptr_t)p; }

/* Copy across the ring boundary (security review §6).
 *
 * Use these rather than validating a pointer and then dereferencing it: a
 * validation is only true at the instant it runs, and any syscall that
 * validates, blocks, and then copies is trusting a snapshot that nothing has
 * refreshed. These validate at the copy and copy fault-recoverably, so the
 * failure is E_BAD_ADDRESS rather than a stale-probe write or a ring-0 fault.
 *
 * `len == 0` is a success that touches nothing, so a NULL/absent buffer with
 * zero length needs no special case at the call site.
 *
 * On failure the destination is PARTIALLY WRITTEN: treat it as undefined.
 */
int  user_copy_out(uint64_t udst, const void *ksrc, uint32_t len);
int  user_copy_in(void *kdst, uint64_t usrc, uint32_t len);

/* The primitive behind both, in usermode.asm: 0 = copied, 1 = faulted and
   recovered. It does NOT validate - it is exported for the self-test, which
   has to reach the recovery path that the validation in user_copy_out exists
   to keep anything from reaching. */
int  user_copy_bytes(void *dst, const void *src, uint32_t len);

/* The recoverable region's bounds and its landing pad (usermode.asm). The
   page-fault handler needs these; nothing else should. */
extern char usercopy_start[];
extern char usercopy_end[];
void usercopy_trampoline(void);

/* How many page faults have been recovered inside that region.
 *
 * This exists because CR0.WP makes the two layers overlap. With WP set, a
 * ring-0 write to a read-only user page faults, and that fault is recoverable,
 * so removing the range check from user_copy_out still yields E_BAD_ADDRESS - the
 * copy just reaches it the other way. The returned value alone can no longer
 * tell which layer refused.
 *
 * The count can. A copy refused by the range check takes NO fault; one refused
 * by recovery takes exactly one. The self-tests assert on the difference, which
 * is the only thing that keeps each layer independently provable. */
uint32_t usercopy_faults_recovered(void);

/* Called only from the page-fault handler, on a recovered fault. */
void usercopy_note_recovered_fault(void);

/* SYS_FILE_* exercised over a real user mapping (see usermode.c).
   1 = pass, 0 = fail. Lives here because it needs map_user_page. */
int  usermode_file_test(void);
int  usermode_flock_test(void);

/* SYS_VM_MAP, SYS_VM_UNMAP and SYS_VM_INFO through the dispatcher, over a
   real user mapping (see usermode.c). 1 = pass, 0 = fail. */
int  usermode_vm_test(void);

/* SYS_INPUT_READ, SYS_INPUT_POLL and SYS_POWER_SLEEP through the dispatcher
   (see usermode.c). 1 = pass, 0 = fail. */
int  usermode_power_input_test(void);

/* The SYS_FB_* calls through the dispatcher, read back from the screen (see
   usermode.c). 1 = pass, 0 = fail. */
int  usermode_fb_test(void);

/* The SYS_NET_* calls through the dispatcher, the configuration read back and
   put back (see usermode.c). 1 = pass, 0 = fail. */
int  usermode_net_test(void);

/* The background console and SYS_LOG_READ (see usermode.c). 1 = pass. */
int  usermode_log_test(void);
int  usermode_process_test(void);   /* process handles, SYS_OBJECT_WAIT, how it ended */
int  usermode_kill_test(void);      /* SYS_PROCESS_KILL: each wait, the safe points */
int  usermode_stop_test(void);      /* SYS_PROCESS_STOP: the notice, the deadline */
int  usermode_job_test(void);       /* jobs: budgets, nesting, a kill of a job */
int  usermode_timer_test(void);     /* the monotonic clock, timer objects, deadlines */
int  usermode_queue_test(void);     /* queue objects: register once, wait for many */
int  usermode_notify_test(void);    /* notifications: bits that combine, signallers */
int  usermode_pipe_test(void);      /* pipes: a ring between two ends */
int  usermode_mem_test(void);       /* memory objects: first touch, strict commit */
int  usermode_ipc_test(void);       /* queued calls, reply tokens, badges */
int  usermode_ipc_handles_test(void);   /* handles in messages: moved, copied, refused */
int  usermode_prio_test(void);          /* priorities, and priority through calls */

/* register the ring-3 fault handler so user faults kill the process, not the
   kernel. call once at boot after the scheduler is up. */
void usermode_register_fault_handler(void);

/* What enter_usermode returns when the ring-3 code faulted instead of calling
   SYS_EXIT. A SYS_EXIT can pass the same number; usermode_faults() tells them
   apart. */
#define USERMODE_FAULTED E_BAD_ADDRESS
/* And when its process was killed (proc_kill). */
#define USERMODE_KILLED  E_CANCELED

/* A safe point: if the current thread's process is being killed, back out of
   enter_usermode with USERMODE_KILLED, and this does not return. The syscall
   gate calls it on its way out, and so does an interrupt taken in ring 3. */
void usermode_kill_point(void);

/* Ring-3 faults taken since boot, for the self-tests. */
uint32_t usermode_faults(void);

/* run the ring-3 demo: enter user mode, run the test routine, return here.
   returns 0 on a clean round-trip. */
int usermode_test(void);

/* For the self-tests: copy `len` bytes of position-independent code to a
   fresh user page, run it in ring 3 with an empty stack and the system page
   mapped (its base in esi or rsi, as a process starts), and return the code
   it gives SYS_EXIT (or USERMODE_FAULTED); -1 when it could not be set up. */
int usermode_run_blob(const uint8_t *start, uint32_t len);

/* Three syscalls made from ring 3 by this thread, as a process makes them
   (the stub in the system page, the arguments in the ABI's registers), for
   the failure sweeps (roadmap 1.6b). Each call is its number and five
   arguments. `data` (at most USERMODE_DATA_MAX bytes) is copied to
   USERMODE_DATA_VIRT, a user page the calls can point into, before ring 3
   runs, and back after. With `fault_nth` nonzero, fault_arm(fault_sites,
   fault_nth) covers ring 3 and nothing of the setting up. Returns what the
   third call returned (or USERMODE_FAULTED; usermode_faults() tells them
   apart), -1 when it could not be set up. */
#define USERMODE_DATA_VIRT  0x00801000u   /* the low half of the stack page */
#define USERMODE_DATA_MAX   0x800u
struct um_call3 { uint64_t c[3][6]; };
int usermode_run_calls(const struct um_call3 *calls, void *data, uint32_t len,
                       uint32_t fault_sites, uint32_t fault_nth);

/* The same three calls made by a held ring-3 process of its own, in this
   thread's job (in the kernel's address space, as every ring-3 helper is;
   its mappings are its own). Returns the process with a reference for the
   caller (proc_put), and its thread in *tid, for thread_start; NULL when it
   could not be made. It ends with what the third call returned. One such
   process at a time: its image is read when it starts. */
struct proc;
struct proc *usermode_calls_process(const struct um_call3 *calls, int *tid);
/* Argument `arg` (1-5) of call `k` (0-2) of the process just made, before
   it starts: for a handle installed in its table after it was made. */
void usermode_calls_set(uint32_t k, uint32_t arg, uint64_t v);

/* The system page: a stub for each syscall and int3 elsewhere; ring 3 calls
   through the base it is handed; a store to the page faults; a call from
   anywhere but its stub is refused. 1 = pass. */
int usermode_syspage_test(void);

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