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

struct ipc_call;

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
    /* Timed sleep. `wake_ns` is the time on timer_now_ns this thread becomes
       runnable again; `sleeping` says the field means anything. */
    uint64_t  wake_ns;
    int       sleeping;
    void    (*entry)(void);    /* what thread_launch calls */
    /* What its creator handed it (thread_set_arg): a record the entry reads,
       and how to free it. Freed when the thread is reaped or discarded, so a
       thread that dies early (a ring-3 fault) leaks nothing of it. */
    void     *arg;
    void    (*arg_drop)(void *arg);
    /* Priority (PRIO_*, cxk_abi.h). `base` is its own (thread_set_base);
       `prio` is what the scheduler runs it at: base, or higher while it
       serves a call from a thread above it (cpu/ipc.c sets it). */
    uint8_t   base;
    uint8_t   prio;
    /* The call it is blocked in, for ipc.c's priority chain; under ipc_lock. */
    struct ipc_call *in_call;
    /* What its handles name (object/kthread.h), a reference; 0 for a thread
       nothing made a handle for. Ended in thread_exit. */
    struct kthread *obj;
    /* It ended itself, through SYS_THREAD_EXIT, with exit_code. */
    uint8_t   self_exit;
    /* Its FS base in ring 3 (SYS_THREAD_SET_TLS): loaded on every switch to
       it. 0 for none, and for every kernel thread. */
    uint64_t  tls_base;
    /* CPU time: what it has run before, in ns, and when it last started
       running, on timer_now_ns (switch_to charges the difference). */
    uint64_t  cpu_ns;
    uint64_t  run_since;
    /* `name` points here once ring 3 has named it (SYS_THREAD_SET_NAME). */
    char      own_name[THREAD_NAME_MAX];
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
   one was left with saved state on freed memory.

   Its new process is in `job`, or with NULL in its creator's job. Returns
   the id; E_CANCELED if that job has been killed, E_NO_RESOURCES if it has
   no thread to spare (cpu/job.h); -1 otherwise. */
struct job;
int  thread_create_process(const char *name, void (*entry)(void), struct job *job);
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
 * running thread (an involuntary yield) every `quantum` milliseconds. Enable only
 * around a contained test for now - not system-wide.
 */
void sched_preempt_enable(uint32_t quantum_ticks);
void sched_preempt_disable(void);

/* called from the timer IRQ; performs a preemptive switch when it's time.
   safe to call always - does nothing unless preemption is enabled. */
void sched_tick(uint64_t now);

/* For the event timer (drivers/char/timer.c), interrupts off: the earliest a
   sleeper is due, and the slice's end if `running` (a halted CPU has no
   slice to end); TIMER_NEVER for nothing. sched_slice_restart starts a whole
   slice when the CPU comes back from a halt. */
uint64_t sched_next_due(int running);
void     sched_slice_restart(uint64_t now);

/* A thread other than the current one is READY, at any priority. */
int      sched_others_ready(void);

/* Priority. The highest level with a thread ready runs; threads at one
   level take turns. A thread starts at PRIO_NORMAL, or its job's ceiling if
   that is lower. thread_set_prio is ipc.c's (the level it runs at, never
   below base); a READY thread raised above the running one is switched to
   at the next preemption point. sched_yield_if_outranked switches now if a
   READY thread is above the current one: the syscall gate's way out. */
uint32_t thread_base(int id);
uint32_t thread_prio(int id);
void     thread_set_base(int id, uint32_t level);
void     thread_set_prio(int id, uint32_t level);
struct ipc_call *thread_in_call(int id);
void     thread_set_in_call(int id, struct ipc_call *c);
void     sched_yield_if_outranked(void);

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

/* A further RING-3 thread in the current thread's process (SYS_THREAD_CREATE):
   HELD, with its esp0 stack, as thread_create_process makes one, so the
   caller fills in its record (thread_set_arg, thread_set_obj) and only then
   lets it go (thread_start). Returns its id; E_CANCELED if the job has been
   killed, E_NO_RESOURCES if it has no thread to spare; -1 otherwise. */
int thread_create_user(const char *name, void (*entry)(void));

/* Give a HELD thread the object its handles name; it takes a reference of
   its own. */
struct kthread;
void thread_set_obj(int id, struct kthread *k);

/* The calling thread is ending itself (SYS_THREAD_EXIT) with `code`: its
   object says THR_EXITED, and if it is the last of its process to end, the
   process ends PROC_EXITED with that code. */
void thread_note_self_exit(int32_t code);
/* a live thread's name, or 0 (for panic reports) */
const char *thread_name(int id);

/* The FS base thread `id` runs ring 3 with (SYS_THREAD_SET_TLS): set on a
   HELD thread before it starts, or on the calling thread, where it takes
   effect at once. The caller has checked it is a user address. */
void     thread_set_tls(int id, uint64_t base);
uint64_t thread_tls(int id);
/* Name the calling thread: `name` (NUL-terminated, shorter than
   THREAD_NAME_MAX) is copied. */
void     thread_set_own_name(const char *name);
/* The CPU time thread `id` has had, in ns, the running slice included; 0 for
   no such thread. Into `name` (THREAD_NAME_MAX bytes), its name. Interrupts
   off, or a lock that keeps the thread from being reaped (OBJINFO_THREAD
   holds the wait lock, which thread_exit's kthread_end takes). */
uint64_t thread_cpu_ns(int id);
void     thread_name_copy(int id, char *name);
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
/* Every wait below can be cut short by a kill (proc_kill): it wakes each
   blocked thread of the process, and none of these blocks a thread whose
   process is being killed - it returns at once. So a caller checks
   thread_killed() when one returns, undoes whatever it registered for the
   wait (a waiter slot, a list entry) and gives up with E_CANCELED; the
   thread then ends on its way back to ring 3. */
void     thread_block(void);            /* block the current thread (IPC wait) + yield */
void     thread_unblock(int id);        /* make a blocked thread runnable again */

/* The current thread's process is being killed (proc_kill). */
int      thread_killed(void);
/* Make every BLOCKED thread of `p` runnable, as thread_unblock does, its
   sleep included. Interrupts off (proc_kill). */
void     sched_wake_process(struct proc *p);
/* Thread `id` is BLOCKED, for the self-tests. */
int      thread_is_blocked(int id);

/* Block until thread_unblock(). Safe when nothing else is runnable at the
   moment: it halts until an interrupt changes that, the way thread_sleep_ms
   does, instead of returning still blocked. thread_block is this too, since
   1.1b; the two names are its callers' (IPC and the keyboard, file locks). */
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
/* The same with an absolute deadline on timer_now_ns (TIMER_NEVER: none). */
void     thread_block_prepare_until(uint64_t deadline);
void     thread_block_finish(void);

/* Block the current thread for `ms` milliseconds, then return. Built on
   thread_block: the thread leaves the run queue entirely, so a sleeping
   process costs nothing but its memory - this is not a spin.

   `ms` of 0 is a yield. The caller may wake EARLY if something else unblocks
   it (an IPC reply, a keystroke, a kill), which is why this returns the tick it
   actually woke at rather than nothing: a caller that must not be cut short
   can check and sleep again. */
uint32_t thread_sleep_ms(uint32_t ms);

/* Called from the timer interrupt: wake any thread whose sleep is due by
   `now`. Only flips state to READY, never switches, so it is safe in an
   interrupt handler - the same contract thread_unblock keeps. */
void     sched_wake_sleepers(uint64_t now);

#endif