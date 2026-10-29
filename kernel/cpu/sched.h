// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/sched.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Cooperative kernel-thread scheduler - Process model, Checkpoint 1.
 *
 * This is the FOUNDATION: it proves the context-switch mechanism in the
 * simplest possible setting - kernel-mode threads (ring 0) that voluntarily
 * yield to each other. No preemption, no ring 3, no separate address spaces
 * yet (those are later checkpoints). Once switching is proven here, preemption
 * and user-mode processes build on top of it.
 *
 * A "thread" here is a kernel function running on its own stack. yield() saves
 * the current thread's context and switches to the next ready thread.
 */

#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include "proc.h"

/* How many threads may be alive is a runtime LIMIT, set from
   /System/Config/kernel.xkco (kconfig.h): policy, not a constant. Thread
   records come from a cache and their ids from a table that grows
   (object/idtab.h), so nothing is sized by the limit; THREAD_LIMIT_MAX only
   bounds what configuration may ask for. Thread ids are below it. Every
   thread also holds a guarded kernel stack and a ring-3 one an esp0 stack
   (memman/kstack.h, KSTACK_SLOTS of them), so asking for more than the
   stacks allow is refused at creation, not at configuration. */
#define THREAD_LIMIT_DEFAULT  8
#define THREAD_LIMIT_MIN      4
#define THREAD_LIMIT_MAX      256

/* Kernel stack size, per thread. Guarded (memman/kstack.h), so a stack that is
   too small is a panic naming the thread rather than silent corruption - which
   is what makes it safe to let configuration choose it. */
#define THREAD_STACK_DEFAULT  8192
#define THREAD_STACK_MIN      8192
#define THREAD_STACK_MAX      32768

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,    /* waiting on IPC; not runnable until unblocked */
    THREAD_HELD,       /* created but not yet started (thread_create_process) */
    THREAD_EXITED
};

struct thread {
    uintptr_t esp;             /* saved kernel stack pointer (THE context) */
    uintptr_t stack_base;      /* guarded kernel stack base, from kstack_alloc (freed on exit) */
    enum thread_state state;
    int       id;              /* thread id (not a pid: see proc.h) */
    struct proc *proc;         /* the process it runs in; holds a reference */
    const char *name;
    int       exit_code;       /* set on exit */
    uintptr_t user_stack_base; /* allocated ring-3 stack (user processes) */
    /* per-process ring-3 return state: enter_usermode saves the kernel esp +
       eflags here so SYS_EXIT (return_to_kernel) can come back, even if another
       ring-3 process runs in between (preemption during ring 3). */
    uintptr_t u_saved_esp;
    uintptr_t u_saved_flags;
    uintptr_t kstack_top;      /* top of this process's esp0 stack -> TSS esp0 */
    uintptr_t kstack_base;     /* guarded esp0 stack base, from kstack_alloc (freed on exit) */
    /* Timed sleep. `wake_tick` is the timer tick this thread becomes runnable
       again; `sleeping` says the field means anything, because tick 0 is a
       real tick and a wrapped counter makes "0 = not sleeping" a lie roughly
       every 49 days. */
    uint32_t  wake_tick;
    int       sleeping;
    void    (*entry)(void);    /* what thread_launch calls */
    /* What its creator handed it (thread_set_arg): a record the entry reads,
       and how to free it. Freed when the thread is reaped or discarded, so a
       thread that dies early (a ring-3 fault) leaks nothing of it. */
    void     *arg;
    void    (*arg_drop)(void *arg);
};

/* initialize the scheduler (registers the currently-running code as thread 0). */
void sched_init(void);

/* create a new kernel thread that starts at entry(). returns its id, or -1. */
int thread_create(const char *name, void (*entry)(void));

/* Create a PROCESS thread, HELD: its kernel stack and its esp0 (ring-0 entry)
   stack both exist, but it will not run until thread_start(). The caller fills
   in everything the thread reads when it starts - image, arguments, address
   space, uid, caps - and only then lets it go.

   thread_create's READY-at-once is wrong for a process. With preemption on,
   the new thread could be scheduled before its creator had given it an esp0
   stack or its records: it entered ring 3 with the TSS still naming another
   thread's stack, and when that thread exited and its stack was freed, this
   one was left with saved state on freed memory. Returns the id, or -1. */
int  thread_create_process(const char *name, void (*entry)(void));
void thread_start(int id);     /* a held thread becomes runnable */

/* Hand a HELD thread a record, and the function that frees it (NULL: nothing
   to free). The thread reads it with thread_current_arg. -1 if `id` is not a
   held thread. These replace the per-thread arrays callers used to keep,
   indexed by thread id. */
int   thread_set_arg(int id, void *arg, void (*drop)(void *arg));
void *thread_current_arg(void);
void thread_discard(int id);   /* a held thread that will never start is freed now */

/* Set the thread limit and the size of stacks made from now on. Refused (-1)
   if out of range, or if more threads are already alive than the new limit. */
int      sched_configure(uint32_t max_threads, uint32_t stack_bytes);
uint32_t sched_thread_limit(void);
uint32_t sched_thread_count(void);   /* slots in use, exited-but-unreaped included */
uint32_t sched_stack_bytes(void);

/* voluntarily give up the CPU to the next ready thread (cooperative). */
void yield(void);

/* terminate the calling thread (never returns). */
void thread_exit(void);

/* how many threads are currently READY or RUNNING (for the demo/tests). */
int sched_active_count(void);

/* ---- preemption (Checkpoint 2) ----
 * When enabled, the timer interrupt calls sched_tick(), which preempts the
 * running thread (an involuntary yield) every `quantum` ticks. Enable only
 * around a contained test for now - not system-wide.
 */
void sched_preempt_enable(uint32_t quantum_ticks);
void sched_preempt_disable(void);

/* called from the timer IRQ; performs a preemptive switch when it's time.
   safe to call always - does nothing unless preemption is enabled. */
void sched_tick(void);

/* called from the IRQ handler AFTER the EOI; performs a deferred preemptive
   switch if one is due. */
void sched_preempt_point(void);

/* the id of the currently running thread. Not a pid: see thread_pid. */
int thread_current_id(void);

/* The process a live thread runs in, and its pid; NULL / -1 for no such
   thread. Every accessor below that takes an id takes a THREAD id and acts on
   that thread's process. */
struct proc *thread_proc(int id);
int          thread_pid(int id);
int          thread_current_pid(void);

/* A further thread in the process of thread `id`, runnable at once. It shares
   the process's handle table, address space, caps, uid and cwd. Kernel mode
   only for now: it gets no esp0 stack. Returns its thread id, or -1. */
int thread_create_sibling(int id, const char *name, void (*entry)(void));
/* a live thread's name, or 0 (for panic reports) */
const char *thread_name(int id);
int thread_is_alive(int id);

/* mark a thread as a user (ring-3) process. */
void thread_mark_user(int id);

/* pointer to the current process's [saved_esp, saved_flags] slot (2 words),
   for enter_usermode/return_to_kernel to stash per-process ring-3 return state. */
uintptr_t *thread_current_usave(void);

/* set/get a process's kernel-stack top (used for the TSS esp0). */
void     thread_set_kstack_top(int id, uintptr_t top);
uintptr_t thread_current_kstack_top(void);

/* allocate a dedicated esp0 (ring-0 entry) stack for a ring-3 process so timer
   preemption / syscalls from ring 3 land on it (not the trampoline stack).
   returns 0 on success, -1 on failure. */
int thread_alloc_kstack(int id);

/* ---- identity (UID) ---- */
/* owning UID of the current process; SYSTEM (0) for kernel/boot context. */
uint32_t thread_current_uid(void);
/* set a thread's owning UID (used when launching a process as a given user). */
void     thread_set_uid(int id, uint32_t uid);

/* ---- working directory ----
 * A CXFS entry id, so resolving a relative path is cxfs_resolve(path, cwd) with
 * no string handling at all. 0 is the root directory, which is also the value a
 * fresh thread starts with, so "no cwd set" and "cwd is /" are the same state
 * and there is nothing to initialise wrongly. Inherited from the creator. */
uint32_t thread_current_cwd(void);
void     thread_set_cwd(int id, uint32_t entry_id);

/* ABI v1 capabilities (cpu/caps.h) */
uint32_t thread_current_caps(void);

/* The handle table of process `id` (object/htab.h), or 0 if there is none. */
struct htab *thread_htab(int id);
/* Its broker handle, as spawn installed it, or -1. */
int32_t  thread_broker(int id);
void     thread_set_broker(int id, int32_t h);
void     thread_set_caps(int id, uint32_t caps);
void     thread_set_space(int id, paddr_t pd_phys);   /* CR3 to load when this thread runs */
void     thread_block(void);            /* block the current thread (IPC wait) + yield */
void     thread_unblock(int id);        /* make a blocked thread runnable again */

/* Block until thread_unblock(). Unlike thread_block, safe when nothing else is
   runnable at the moment: it halts until an interrupt changes that, the way
   thread_sleep_ms does, instead of returning still blocked. For waits whose
   waker is another thread that may itself be asleep (SYS_FILE_LOCK). */
void     thread_wait(void);

/* A block in two halves, for a waiter that checks a condition under a lock
   and must not miss a wake between the check and the block (object/kwait.c).
   thread_block_prepare, with interrupts off (the lock held): the current
   thread is BLOCKED from here, and if `ms` is not 0 a timer wakes it that many
   milliseconds on. A thread_unblock after this point is not lost, even one
   that lands before thread_block_finish. Then let go of the lock and call
   thread_block_finish, which returns once the thread is runnable again,
   whoever woke it. */
void     thread_block_prepare(uint32_t ms);
void     thread_block_finish(void);

/* Block the current thread for `ms` milliseconds, then return. Built on
   thread_block: the thread leaves the run queue entirely, so a sleeping
   process costs nothing but its memory - this is not a spin.

   `ms` of 0 is a yield. The caller may wake EARLY if something else unblocks
   it (an IPC reply, a keystroke), which is why this returns the tick it
   actually woke at rather than nothing: a caller that must not be cut short
   can check and sleep again. */
uint32_t thread_sleep_ms(uint32_t ms);

/* Called from the timer interrupt once per tick: wake any thread whose sleep
   has expired. Only flips state to READY, never switches, so it is safe in an
   interrupt handler - the same contract thread_unblock keeps. */
void     sched_wake_sleepers(void);

#endif