/* /CXK/kernel/cpu/sched.h */
/* Aurora Tejeda / CATX Systems LLC */
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

#define MAX_THREADS   8
#define THREAD_STACK  8192      /* per-thread kernel stack size */

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_EXITED
};

struct thread {
    uint32_t esp;              /* saved kernel stack pointer (THE context) */
    uint32_t stack_base;       /* allocated kernel stack (for free on exit) */
    enum thread_state state;
    int       id;              /* PID */
    const char *name;
    int       is_user;         /* 1 = runs in ring 3 (a user process) */
    int       exit_code;       /* set on exit */
    uint32_t  uid;             /* owning user id; 0 = SYSTEM (machine identity) */
    uint32_t  user_stack_base; /* allocated ring-3 stack (user processes) */
    /* per-process ring-3 return state: enter_usermode saves the kernel esp +
       eflags here so SYS_EXIT (return_to_kernel) can come back, even if another
       ring-3 process runs in between (preemption during ring 3). */
    uint32_t  u_saved_esp;
    uint32_t  u_saved_flags;
    uint32_t  kstack_top;      /* top of this process's esp0 stack -> TSS esp0 */
    uint32_t  kstack_base;     /* allocated esp0 stack (for free on exit) */
};

/* initialize the scheduler (registers the currently-running code as thread 0). */
void sched_init(void);

/* create a new kernel thread that starts at entry(). returns its id, or -1. */
int thread_create(const char *name, void (*entry)(void));

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

/* the id (pid) of the currently running thread/process. */
int thread_current_id(void);

/* mark a thread as a user (ring-3) process. */
void thread_mark_user(int id);

/* pointer to the current process's [saved_esp, saved_flags] slot (2 words),
   for enter_usermode/return_to_kernel to stash per-process ring-3 return state. */
uint32_t *thread_current_usave(void);

/* set/get a process's kernel-stack top (used for the TSS esp0). */
void     thread_set_kstack_top(int id, uint32_t top);
uint32_t thread_current_kstack_top(void);

/* allocate a dedicated esp0 (ring-0 entry) stack for a ring-3 process so timer
   preemption / syscalls from ring 3 land on it (not the trampoline stack).
   returns 0 on success, -1 on failure. */
int thread_alloc_kstack(int id);

/* ---- identity (UID) ---- */
/* owning UID of the current process; SYSTEM (0) for kernel/boot context. */
uint32_t thread_current_uid(void);
/* set a thread's owning UID (used when launching a process as a given user). */
void     thread_set_uid(int id, uint32_t uid);

#endif