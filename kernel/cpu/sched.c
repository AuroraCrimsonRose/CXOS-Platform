// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/sched.c */
/* Aurora Tejeda / CATX Systems */
/* Cooperative kernel-thread scheduler (Checkpoint 1). */

#include "sched.h"
#include "heap.h"
#include "kstack.h"
#include "arch/cpu.h"
#include "arch/cpu_ops.h"
#include "arch/paging.h"
#include "uid.h"
#include "timer.h"   /* timer_ticks for the sleep queue */
#include "object/idtab.h"
#include "memman/ocache.h"
#include "panic.h"
#include "cxk_abi.h"   /* E_OK: a job's refusal; PRIO_* */
#include "job.h"       /* job_prio_ceiling */
#include "ipc.h"       /* ipc_thread_exit */
#include "object/kthread.h"   /* kthread_end: a thread's handles */
#include "sync/spinlock.h"   /* sched_lock */
#include "sync/lockorder.h"  /* no lock but sched_lock across a switch */
#include "memman/physmap.h"  /* nor a mapped frame */
#include "percpu.h"          /* the running thread is the CPU's */
#include "arch/smp.h"        /* 1.9e2: the IPI that hands a CPU a thread */


static void sched_reap(void);   /* forward decl: used by yield, defined below */
struct thread;
static void kick(struct thread *t, int this_cpu_too);   /* 1.9e2: hand a ready thread a CPU */

/* Every thread is a record from its own cache, named by an id from `tids`.
   The id is the index the old fixed array had: small, lowest free first,
   0 for the boot context. The table grows as threads are made, so the limit
   is configuration (sched_configure) and not the size of an array. */
static struct ocache thread_cache;
static struct idtab  tids;

/* The id of the running thread: the CPU's own (percpu.h, 1.9b), read in one
   load, so a thread moved between CPUs mid-read never sees another's.
   Written only by switch_to and sched_init, under sched_lock. */
#define current cpu_thread()

/* Another thread's record: under sched_lock only (1.9e2). The table grows
   under the lock, freeing its old array, and a record goes back once it is
   out of the table; read without the lock, either can go from under the
   reader on another CPU. */
static inline struct thread *th(int id) { return (struct thread *)idtab_get(&tids, id); }
/* The running thread's own, which nothing frees while it runs, through the
   CPU's pointer in one load (percpu.h): never through the table. */
static inline struct thread *self(void) { return cpu_thread_rec(); }

/* A live thread (allocated, exited-unreaped included), or NULL. */
static inline struct thread *live(int id) {
    struct thread *t = th(id);
    return (t && t->state != THREAD_UNUSED) ? t : 0;
}
static uint64_t cpu_ns_of(const struct thread *t);
static int initialized = 0;

/* The scheduler's lock (1.9b; sync/lockorder.h LR_SCHED, after kwait, whose
   wakes come here). It took the place of the interrupts-off sections, which
   were the whole of the guard on one CPU and are none on two. It guards
   every thread's state, the id table and the run order, and it is held
   ACROSS a switch: taken by the thread that switches out, given back by the
   one that switches in where it resumes (yield, sched_preempt_point) or,
   the first time it runs, in thread_launch. Given back any earlier, a
   second CPU could pick the outgoing thread and run it on the stack it is
   still standing on. Serenity's g_scheduler_lock goes across the switch
   the same way (Kernel/Tasks/Scheduler.cpp, leave_on_first_switch).
   switch_to checks it is held, and nothing else is.

   Nothing is taken under it but what the lock order puts after it: a
   thread's resources go back after the reaper has let it go (sched_reap),
   since their locks (kwait, pids) rank before it. A halt gives it back
   first, with interrupts kept off (sched_unlock_irqs_off), since the tick
   that ends the halt takes it.

   Interrupts off across a switch, which the lock keeps. yield() and
   thread_exit() used to run with them on, and a timer tick landing after `current = next` but before
   context_switch ran the preemption path with `current` already naming the NEW
   thread while the CPU was still on the OLD thread's stack: it saved the old
   stack pointer as the new thread's context. The old thread was often exiting,
   so its stack was freed next, and the new thread later resumed on freed
   memory. With heap stacks that read back the right bytes by luck and quietly
   corrupted whatever reused them; with guarded stacks the pages are unmapped,
   and it faults - which is how it was found. The preemption path already runs
   with interrupts off (it is inside the IRQ handler); now the other two do.

   Saved and restored rather than cli/sti: the flags context_switch pushes are
   the ones a thread resumes with, and a caller that already had interrupts
   off must get them back off. */
static spinlock_t sched_lock = SPINLOCK_INIT(LR_SCHED);

static inline uint32_t irq_save(void)          { return arch_irq_save(); }
static inline void     irq_restore(uint32_t f) { arch_irq_restore(f); }

/* Every take and give of sched_lock goes through these, so a test-hook
   build counts its contention (sync/lockstat.h, 1.9f): the profile the
   choice between this one order and a run queue per CPU waits on. */
static inline uint32_t sched_take(void)       { return spin_lock_counted(&sched_lock, LOCKSTAT_SCHED); }
static inline void     sched_give(uint32_t f) { spin_unlock_counted(&sched_lock, f, LOCKSTAT_SCHED); }

/* Give sched_lock back and leave interrupts off, whatever the taker had:
   a halt or a reap must not be cut into by the tick that wants the lock. */
static inline void sched_unlock_irqs_off(void) { sched_give(irq_save()); }

/* A thread's record for reading or writing one of its fields (1.9e2: the
   per-thread reads 1.9b left without the lock). The running thread's own
   needs no lock; any other is found under sched_lock, held until
   look_done, so it is neither freed nor its table moved meanwhile. What a
   field points at (its process) outlives the call only as long as the
   thread does: the caller's to know. */
#define LOOK_SELF 0xFFFFFFFFu   /* no flags word is all ones: no lock taken */
static struct thread *look(int id, uint32_t *f) {
    if (id == current) { *f = LOOK_SELF; return self(); }
    *f = sched_take();
    return live(id);
}
static inline void look_done(uint32_t f) { if (f != LOOK_SELF) sched_give(f); }

static uint32_t thread_limit = THREAD_LIMIT_DEFAULT;
static uint32_t stack_bytes  = THREAD_STACK_DEFAULT;

/* Threads that hold an id, EXITED ones included: an exited thread still holds
   its stacks until the reaper runs. Counting them keeps the limit honest, and
   because a new thread takes the lowest free id, it also keeps every thread
   id below the limit. */
static uint32_t threads_allocated(void) { return tids.used; }

int sched_configure(uint32_t max_threads, uint32_t bytes) {
    if (max_threads < THREAD_LIMIT_MIN || max_threads > THREAD_LIMIT_MAX) return -1;
    if (bytes < THREAD_STACK_MIN || bytes > THREAD_STACK_MAX || bytes % 4096u) return -1;
    if (initialized && threads_allocated() > max_threads) return -1;
    thread_limit = max_threads;
    stack_bytes  = bytes;
    return 0;
}

uint32_t sched_thread_limit(void) { return thread_limit; }
uint32_t sched_thread_count(void) { return threads_allocated(); }
uint32_t sched_stack_bytes(void)  { return stack_bytes; }

/* trampoline: the first thing a new thread runs. The thread that switched
   here holds sched_lock, with interrupts off (arch_thread_frame): it is
   given back, then interrupts go on, as every thread starts. Then its
   entry, then exit. */
static void thread_launch(void) {
    sched_unlock_irqs_off();
    arch_irq_enable();
    void (*entry)(void) = self()->entry;
    if (entry) entry();
    thread_exit();
}

static __attribute__((noreturn)) void idle_loop(void);

/* CPU `c`'s idle thread (1.9e2): a record from the thread cache, outside the
   id table, so no walk of threads, count or limit sees it, its id past
   every thread's. The boot CPU's has a stack of its own and starts at
   idle_loop on its first switch; an AP's is the context the AP starts in,
   which sched_cpu_enter turns into it. WRK's KiSelectNextThread picks a
   processor's idle thread when nothing is ready (ke/thredsup.c), as
   Serenity's pull_next_runnable_thread does (Kernel/Tasks/Scheduler.cpp);
   before it, a CPU with nothing to run halted on whatever thread had just
   blocked, which on two CPUs is a thread another CPU may wake and run on
   the stack this one is standing on. */
#define IDLE_STACK_BYTES 8192u
int sched_cpu_prepare(struct cpu *c) {
    struct thread *t = ocache_alloc(&thread_cache);   /* zeroed */
    if (!t) return -1;
    t->id = THREAD_IDLE_ID + (int)c->index;
    if (c == &cpu_boot) {
        uintptr_t base = 0, top = kstack_alloc(IDLE_STACK_BYTES, t->id, &base);
        if (!top) { ocache_free(&thread_cache, t); return -1; }
        t->stack_base = base;
        t->esp = arch_thread_frame(top, thread_launch);
    }
    t->name     = "idle";
    t->entry    = idle_loop;
    t->state    = THREAD_READY;
    t->idle     = 1;
    t->affinity = 1u << c->index;
    c->idle_thread = t;
    if (c != &cpu_boot) { c->cur = t; c->thread = t->id; }   /* from its first instruction */
    return 0;
}

void sched_cpu_unprepare(struct cpu *c) {
    struct thread *t = c->idle_thread;
    if (!t || c == &cpu_boot) return;
    c->idle_thread = 0;
    c->cur = 0;
    ocache_free(&thread_cache, t);
}

void sched_init(void) {
    if (ocache_init(&thread_cache, "thread", sizeof(struct thread)) != 0)
        kpanic("sched_init: no thread cache");
    idtab_init(&tids, THREAD_LIMIT_MAX);
    proc_sys_init();
    struct thread *t0 = ocache_alloc(&thread_cache);   /* zeroed */
    if (!t0 || idtab_alloc(&tids, t0, 0) != 0) kpanic("sched_init: no thread 0");
    t0->id = 0;
    /* thread 0 = the currently-running kernel context. Its esp is filled in on
       the first switch away from it (context_switch saves into &th(0)->esp). */
    th(0)->state = THREAD_RUNNING;
    th(0)->name  = "main";
    th(0)->base  = th(0)->prio = PRIO_NORMAL;
    th(0)->stack_base = 0;   /* uses the existing kernel stack */
    th(0)->affinity   = THREAD_ON_BOOT_CPU;
    th(0)->on_cpu     = 1;
    cpu_boot.cur      = th(0);
    if (sched_cpu_prepare(&cpu_boot) != 0) kpanic("sched_init: no idle thread");
    /* the boot/kernel context is the machine (User 0): proc_create with no
       parent gives exactly that */
    th(0)->proc  = proc_create(0, 0);
    if (!th(0)->proc) return;   /* initialized stays 0: nothing schedules */
    proc_thread_in(th(0)->proc);   /* the root job: no limit to refuse it */
    proc_put(th(0)->proc);      /* the thread's reference is the one that counts */
    cpu_this()->thread = 0;
    initialized = 1;
}

static int thread_create_in(const char *name, void (*entry)(void), int held, struct proc *into,
                            struct job *job);

int thread_create(const char *name, void (*entry)(void)) {
    int t = thread_create_in(name, entry, 0, 0, 0);
    return t < 0 ? -1 : t;
}

/* HELD until its CPUs are set, so no CPU outside them ever picks it. */
int thread_create_on(const char *name, void (*entry)(void), uint32_t mask) {
    int t = thread_create_in(name, entry, 1, 0, 0);
    if (t < 0) return -1;
    if (thread_set_affinity(t, mask) != 0) { thread_discard(t); return -1; }
    thread_start(t);
    return t;
}

int thread_create_sibling(int id, const char *name, void (*entry)(void)) {
    struct proc *p = thread_proc(id);
    if (!p) return -1;
    int t = thread_create_in(name, entry, 0, p, 0);
    return t < 0 ? -1 : t;
}

/* A thread that will run ring-3 code goes to any CPU (1.9e4): its syscalls
   copy user memory, and what they touch is locked. Set while it is HELD, so
   no CPU picks it before. */
int thread_create_process(const char *name, void (*entry)(void), struct job *job) {
    int id = thread_create_in(name, entry, 1, 0, job);
    if (id < 0) return id;
    if (thread_alloc_kstack(id) != 0 || thread_set_affinity(id, THREAD_ON_ANY_CPU) != 0) {
        thread_discard(id);
        return -1;
    }
    return id;
}

int thread_create_user(const char *name, void (*entry)(void)) {
    if (!initialized || !self()->proc) return -1;
    int id = thread_create_in(name, entry, 1, self()->proc, 0);
    if (id < 0) return id;
    if (thread_alloc_kstack(id) != 0 || thread_set_affinity(id, THREAD_ON_ANY_CPU) != 0) {
        thread_discard(id);
        return -1;
    }
    return id;
}

void thread_set_obj(int id, struct kthread *k) {
    uint32_t f;
    struct thread *t = look(id, &f);
    if (id > 0 && t && t->state == THREAD_HELD && !t->obj) {
        kobj_ref(&k->hdr);
        t->obj = k;
    }
    look_done(f);
}

void thread_note_self_exit(int32_t code) {
    if (!initialized) return;
    self()->self_exit = 1;
    self()->exit_code = code;
}

void thread_start(int id) {
    uint32_t f = sched_take();
    struct thread *t = th(id);
    /* Not this CPU's need_resched: only thread context starts a thread, and
       a higher one runs when the starter next yields (ring 3: the syscall
       gate's sched_yield_if_outranked). need_resched acts at an interrupt.
       Another CPU, idle or running something lower, is handed it. */
    if (id > 0 && t && t->state == THREAD_HELD) {
        t->state = THREAD_READY;
        kick(t, 0);
    }
    sched_give(f);
}

int thread_set_arg(int id, void *arg, void (*drop)(void *arg)) {
    uint32_t f;
    struct thread *t = look(id, &f);
    int rc = -1;
    if (id > 0 && t && t->state == THREAD_HELD) {
        t->arg      = arg;
        t->arg_drop = drop;
        rc = 0;
    }
    look_done(f);
    return rc;
}

void *thread_current_arg(void) {
    return initialized ? self()->arg : 0;
}

/* Everything a thread holds goes back, then its id and its record. Never the
   running thread: a thread cannot free the stack it stands on. */
static void thread_free(struct thread *t) {
    if (t->stack_base)      { kstack_free(t->stack_base);  t->stack_base = 0; }
    if (t->user_stack_base) { kfree((void *)t->user_stack_base); t->user_stack_base = 0; }
    if (t->kstack_base)     { kstack_free(t->kstack_base); t->kstack_base = 0; }
    t->kstack_top = 0;
    /* What its creator handed it (thread_set_arg), however it ended. */
    if (t->arg_drop) t->arg_drop(t->arg);
    t->arg = 0;
    t->arg_drop = 0;
    /* A thread that never ran ends here, with its process (it had no say). */
    if (t->obj) {
        kthread_end(t->obj, THR_WITH_PROCESS, 0, t->cpu_ns, t->name);
        kobj_unref(&t->obj->hdr);
        t->obj = 0;
    }
    t->self_exit = 0;
    /* One that never ran stops here; one that ran stopped in thread_exit. */
    if (t->state == THREAD_HELD) proc_thread_stopped(t->proc, 0);
    proc_thread_gone(t->proc);
    t->proc  = 0;
    /* Out of the table under the lock, which every walk of it holds; the
       record goes back after, when nothing can find it. */
    uint32_t f = sched_take();
    t->state = THREAD_UNUSED;
    idtab_take(&tids, t->id);
    sched_give(f);
    ocache_free(&thread_cache, t);
}

/* Never ran, so nothing is standing on its stacks: free them here rather than
   leave it to the reaper, which only collects EXITED threads. With
   interrupts off, as the reaper's free is. */
void thread_discard(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    int held = id > 0 && t && t->state == THREAD_HELD;
    look_done(f);
    /* Only its creator frees a HELD thread, so it is still there below. */
    if (!held) return;
    f = irq_save();
    thread_free(t);
    irq_restore(f);
}

static int thread_create_in(const char *name, void (*entry)(void), int held, struct proc *into,
                            struct job *job) {
    if (!initialized) return -1;
    if (threads_allocated() >= thread_limit) return -1;

    /* Zeroed, and UNUSED until the end: the timer interrupt can see it in the
       table from here on, and must not run it half-built. An EXITED thread
       keeps its id until it is reaped, so its stacks are never clobbered. */
    struct thread *t = ocache_alloc(&thread_cache);
    if (!t) return -1;
    uint32_t f = sched_take();
    int slot = idtab_alloc(&tids, t, 1);
    sched_give(f);
    if (slot < 0) { ocache_free(&thread_cache, t); return -1; }
    t->id = slot;

    /* A guarded stack (memman/kstack.h): running off the bottom faults rather
       than overwriting the heap, which is what a kmalloc'd stack did. */
    /* Its process: the one it joins, or a new one that inherits the
       creator's uid and cwd and nothing else, in `job` or the creator's. */
    struct proc *p = into ? into : proc_create(self()->proc, job);
    /* Named after its first thread until something better is known of it
       (proc_start: the file a program came from). */
    if (p && !into) proc_set_name(p, name);
    uintptr_t stack = 0;
    uintptr_t stack_top = p ? kstack_alloc(stack_bytes, slot, &stack) : 0;
    /* Its job's say last: it may be killed, or have no thread to spare. */
    int rc = stack_top ? proc_thread_in(p) : -1;
    if (rc != E_OK) {
        if (stack_top) kstack_free(stack);
        if (p && !into) proc_put(p);
        f = sched_take();
        idtab_take(&tids, slot);
        sched_give(f);
        ocache_free(&thread_cache, t);
        return rc;
    }
    if (!into) proc_put(p);   /* the new thread's reference keeps it */

    /* The new stack must look exactly like a thread that context_switch
       switched out, so the first switch into it lands at thread_launch. */
    t->esp        = arch_thread_frame(stack_top, thread_launch);
    t->stack_base = stack;
    t->name       = name;
    t->proc       = p;
    t->exit_code  = 0;
    t->user_stack_base = 0;
    t->u_saved_esp   = 0;
    t->u_saved_flags = 0;
    /* kstack_top 0: a kernel-only thread, which never enters ring 3 and so runs
       on the dedicated default stack. A thread that WILL enter ring 3 is made
       with thread_create_process, which gives it its own esp0 before it can be
       scheduled (PROCESS_MODEL.md §8). */
    t->kstack_top  = 0;
    t->kstack_base = 0;
    t->entry       = entry;
    t->arg         = 0;
    t->arg_drop    = 0;
    /* PRIO_NORMAL, or lower in a job whose ceiling is: SYS_THREAD_SET_PRIORITY
       could not raise it back past that. */
    uint32_t ceil = job_prio_ceiling(p->job);
    t->base        = (uint8_t)(ceil < PRIO_NORMAL ? ceil : PRIO_NORMAL);
    t->prio        = t->base;
    t->in_call     = 0;
    t->obj         = 0;
    t->self_exit   = 0;
    t->tls_base    = 0;
    t->cpu_ns      = 0;
    t->run_since   = 0;
    t->affinity    = THREAD_ON_BOOT_CPU;   /* a kernel thread's: sched.h */
    t->on_cpu      = 0;
    t->idle        = 0;
    /* Last, under the lock: from here the timer, or another CPU, may run it. */
    f = sched_take();
    t->state = held ? THREAD_HELD : THREAD_READY;
    if (!held) kick(t, 0);
    sched_give(f);
    return slot;
}

/* May `t` run here, now? READY, on no CPU, and this CPU in its affinity; the
   running thread itself while it is still runnable. sched_lock held. A
   READY thread can still be on a CPU: one woken between marking itself
   BLOCKED and switching away (thread_wait) is READY on its own CPU until
   that CPU's next pick, and another CPU running it there would run it twice
   on one stack. Serenity marks a thread active for the same reason
   (Scheduler::pull_next_runnable_thread). */
static int runnable_here(const struct thread *t, const struct cpu *c) {
    if (!(t->affinity & (1u << c->index))) return 0;
    if (t == c->cur) return t->state == THREAD_READY || t->state == THREAD_RUNNING;
    return t->state == THREAD_READY && !t->on_cpu;
}

/* What this CPU runs next: the runnable thread at the highest priority, the
   first after the running one among those at it: round-robin within a
   level, and the running one (last in the scan) only if no other at its
   level is ready; this CPU's idle thread if none. One order for every CPU
   under the one lock, as Serenity's ready queues are, rather than a queue
   per CPU (Haiku's CPUEntry, WRK's per-processor ready lists): a scan of
   every thread, short under the thread limit, until a profile of the SMP
   stress suite (1.9f) says otherwise. sched_lock held. */
static struct thread *next_runnable(void) {
    struct cpu *c = cpu_this();
    struct thread *best = 0;
    int cap = (int)tids.cap, top = -1;
    int from = c->cur->idle ? cap - 1 : c->cur->id;   /* idle: from the first */
    for (int n = 1; n <= cap; n++) {
        struct thread *t = th((from + n) % cap);
        if (t && runnable_here(t, c) && (int)t->prio > top) {
            top  = t->prio;
            best = t;
        }
    }
    return best ? best : c->idle_thread;
}

/* Ask CPU `c` for a switch at its next preemption point, or its idle loop's
   next look; and if it is another CPU, the IPI that ends its halt or
   interrupts its thread. Once per ask: a CPU already asked has an IPI on
   its way (Haiku's invoke_scheduler, scheduler.cpp enqueue). */
static void resched(struct cpu *c) {
    if (__atomic_exchange_n(&c->need_resched, 1u, __ATOMIC_ACQ_REL)) return;
    if (c != cpu_this()) {
        c->resched_ipis++;
        smp_kick(c->index);
    }
}

/* `t` was just made READY or raised: hand it a CPU that may run it and
   should, at once rather than at the end of a slice: one idle, or else the
   one running the lowest thread below it. None: it waits its turn. This
   CPU is asked only with `this_cpu_too`, as thread_start never asked it.
   Serenity wakes an idle processor the same way
   (smp_wake_n_idle_processors, Arch/x86_64/Processor.cpp); Haiku hands a
   thread to the CPU whose running priority is lowest (enqueue). sched_lock
   held, so each CPU's running thread holds still. */
static void kick(struct thread *t, int this_cpu_too) {
    if (t->state != THREAD_READY || t->on_cpu) return;
    struct cpu *me = cpu_this(), *pick = 0;
    int low = t->prio;
    for (unsigned i = 0; i < cpu_count(); i++) {
        struct cpu *c = cpu_at(i);
        if (!(t->affinity & (1u << i)) || !c->cur) continue;
        if (c == me && !this_cpu_too) continue;
        if (c->cur == c->idle_thread) { pick = c; break; }
        if ((int)c->cur->prio < low) { low = c->cur->prio; pick = c; }
    }
    if (pick) resched(pick);
}

/* Point the stack the CPU takes on entry from ring 3 (arch/cpu.h; the TSS on
   x86) at the current thread's esp0 stack, so a
   syscall/interrupt arriving while THIS process is in ring 3 lands on its own
   stack, never another process's. Called on every context switch.

   THE esp0 INVARIANT (PROCESS_MODEL.md §8): after every switch, tss.esp0 is
   the running thread's kstack_top, or the dedicated default stack when it has
   none. A kernel-only thread used to leave tss.esp0 untouched - still pointing
   at the previous process's esp0 stack, despite this comment saying otherwise.
   Harmless only while no kernel-only thread enters ring 3, which nothing
   checked: one that did would have pushed its frames onto a live process's
   saved interrupt frame. The self-test "esp0 follows the running thread"
   holds it. */
static void update_kernel_stack(void) {
    uintptr_t top = self()->kstack_top;
    arch_kernel_stack_set(top ? top : arch_kernel_stack_default());
}

/* Make the current thread's address space the active one. Threads with pd_phys 0
   (kernel threads) run in the shared kernel space. Called on every switch,
   right after update_kernel_stack(). The kernel half is mapped in every space,
   so this is safe for kernel threads regardless of which space is loaded. */
static void update_address_space(void) {
    paddr_t mine = self()->proc ? self()->proc->pd_phys : 0;
    addr_space_switch(mine ? mine : addr_space_kernel());
}

/* `next` runs from here, in place of the current thread: everything a switch
   changes but the stack, which context_switch does after. The three switch
   sites (yield, sched_preempt_point, thread_exit) all come through here, so
   a thread never runs with another's esp0, address space or FS base.
   sched_lock held, and nothing else (it alone goes across the switch).

   CPU time is charged here, by the clock, as Serenity's
   Thread::update_time_scheduled does, rather than sampled a tick at a time
   (WRK's KeUpdateRunTime): a thread that blocks inside every tick is
   charged what it ran, not nothing. Interrupts are charged to whoever they
   arrived on. */
static void slice_begin(struct cpu *c, const struct thread *next, uint64_t now);

static void switch_to(struct thread *next) {
#if CXK_LOCK_CHECK
    /* A spinlock is the CPU's, never carried into another thread: all but
       this one, which the thread switched to gives back. */
    lockorder_assert_switch(&sched_lock, "thread switch holding");
#endif
    /* So is a frame mapped through memman/physmap.h, in every build. */
    phys_assert_none("thread switch with a frame mapped:");
    struct cpu *c = cpu_this();
    uint64_t now = timer_now_ns();
    struct thread *prev = self();
    if (now > prev->run_since) prev->cpu_ns += now - prev->run_since;
    /* Off this CPU, and the next on it: both seen by another CPU only once
       the thread switched to gives sched_lock back, after the stack has
       changed (1.9e2). */
    prev->on_cpu = 0;
    next->on_cpu = c->index + 1;
    /* Switched out still ready (a slice, an outranking wake, an affinity it
       no longer meets here): another CPU it may use may be idle. That CPU
       takes it only once the lock comes back, after its stack is saved. */
    if (prev->state == THREAD_READY && !prev->idle) kick(prev, 0);
    next->run_since = now;
    next->state = THREAD_RUNNING;
    c->cur    = next;
    c->thread = next->id;
    update_kernel_stack();
    update_address_space();
    arch_tls_base_set(self()->tls_base);
    slice_begin(c, next, now);
}

/* ---- each CPU's idle thread (1.9e2) ----
 * Runs when nothing else may run on its CPU: picks again whenever it wakes,
 * and halts until an interrupt when there is still nothing. Halting is the
 * boot CPU's timer_idle, which sets the event timer for the next deadline,
 * or an AP's smp_idle_wait (arch/smp.h), which runs a call left for it and
 * marks the CPU idle for the TLB shootdown. Both are entered with
 * interrupts off from the look on: a thread handed to this CPU after the
 * look comes with an IPI, which is then taken in the halt and ends it.
 * Idle time is the idle thread's (cpu_ns), WRK's rule, where before it was
 * no one's. Exited threads are reaped here too, on every CPU (1.9e3). */
static __attribute__((noreturn)) void idle_loop(void) {
    struct cpu *c = cpu_this();   /* an idle thread never moves */
    struct thread *me = c->idle_thread;
    for (;;) {
        arch_irq_disable();
        sched_reap();
        sched_take();
        __atomic_store_n(&c->need_resched, 0u, __ATOMIC_RELAXED);
        struct thread *next = next_runnable();
        if (next != me) {
            switch_to(next);
            context_switch(&me->esp, next->esp);
            sched_unlock_irqs_off();   /* handed over by whoever switched back */
            continue;
        }
        sched_unlock_irqs_off();
        if (c == &cpu_boot) timer_idle();
        else                smp_idle_wait();
    }
}

void sched_cpu_enter(void) {
    struct cpu *c = cpu_this();
    struct thread *me = c->idle_thread;
    arch_irq_disable();
    sched_take();
    me->on_cpu    = c->index + 1;
    me->state     = THREAD_RUNNING;
    me->run_since = timer_now_ns();
    update_kernel_stack();
    update_address_space();
    arch_tls_base_set(0);
    sched_unlock_irqs_off();
    idle_loop();
}

void thread_set_space(int id, paddr_t pd_phys) {
    struct proc *p = thread_proc(id);
    if (p) p->pd_phys = pd_phys;
}

static inline int killing(const struct thread *t) { return t->proc && t->proc->killing; }

/* BLOCKED unless its process is being killed, under sched_lock from the
   check to the state, as proc_kill wakes under it after setting `killing`:
   a kill between the two would find this thread still RUNNING, wake
   nobody, and leave it blocked for good once it got back. */
static int block_unless_killed(void) {
    uint32_t f = sched_take();
    int go = !killing(self());
    if (go) self()->state = THREAD_BLOCKED;
    sched_give(f);
    return go;
}

/* Block the current thread (e.g. waiting on an IPC reply) and switch away.
   Returns once thread_unblock() has made it runnable and it is scheduled
   again. It used to be a bare yield, which said its callers must leave
   another thread runnable. An IPC receiver with no caller does not: yield
   came straight back, the receive loop blocked again, and with interrupts
   off for the call no tick could wake anyone - a machine whose only other
   thread slept hung (seen in 1.1b's kill test). thread_wait idles instead. */
void thread_block(void) {
    thread_wait();
}

int thread_killed(void) { return initialized && killing(self()); }

void sched_wake_process(struct proc *p) {
    uint32_t f = sched_take();
    for (uint32_t i = 0; i < tids.cap; i++) {
        struct thread *t = th((int)i);
        if (!t || t->proc != p || t->state != THREAD_BLOCKED) continue;
        t->sleeping = 0;
        t->state    = THREAD_READY;
        kick(t, 0);
    }
    sched_give(f);
}

int thread_is_blocked(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    int blocked = t && t->state == THREAD_BLOCKED;
    look_done(f);
    return blocked;
}

/* RUNNING again, once woken and back. */
static void running_again(void) {
    uint32_t f = sched_take();
    self()->sleeping = 0;
    self()->state    = THREAD_RUNNING;
    sched_give(f);
}

/* A BLOCKED thread's yield always switches away, to the idle thread if
   nothing else (1.9e2), and comes back here once woken and picked. It used
   to stay when nothing else could run and halt on its own stack, which on
   two CPUs let another CPU wake it and run it on that same stack. */
void thread_wait(void) {
    if (!initialized) return;
    if (!block_unless_killed()) return;
    while (self()->state == THREAD_BLOCKED) yield();
    running_again();
}

void thread_unblock(int id) {
    uint32_t f = sched_take();
    struct thread *t = th(id);
    if (t && t->state == THREAD_BLOCKED) {
        t->state = THREAD_READY;
        kick(t, 1);
    }
    sched_give(f);
}

/* ---- timed sleep ----
 *
 * A sleeping thread leaves the run queue the same way an IPC waiter does, so
 * it costs nothing but its memory. The timer interrupt puts it back.
 */
uint32_t thread_sleep_ms(uint32_t ms) {
    if (!initialized || ms == 0) { yield(); return timer_ticks(); }
    thread_block_prepare(ms);
    thread_block_finish();
    return timer_ticks();
}

void thread_block_prepare(uint32_t ms) {
    thread_block_prepare_until(ms ? timer_now_ns() + (uint64_t)ms * TIMER_NS_PER_TICK
                                  : TIMER_NEVER);
}

void thread_block_prepare_until(uint64_t deadline) {
    if (!initialized) return;
    /* All three at once, under sched_lock, which the tick's wake takes too.
       Set one at a time, a tick landing between `sleeping` and `state` found a thread
       already due but still RUNNING: it cleared `sleeping` and left the state
       alone, the thread then blocked itself, and nothing would ever wake it.
       A 1 ms sleep is due at the very next tick, so the window was real.
       Not at all for a thread being killed: thread_block_finish then
       returns at once. The timer's hint takes no lock. */
    uint32_t f = sched_take();
    if (!killing(self())) {
        if (deadline < TIMER_NEVER) {
            self()->wake_ns  = deadline;
            self()->sleeping = 1;
        }
        self()->state = THREAD_BLOCKED;
        if (deadline < TIMER_NEVER) timer_deadline_hint(deadline);
    }
    sched_give(f);
}

/* As thread_wait: the timer or a thread_unblock makes it READY, and the
   yield that switched it away comes back. With nothing else to run, its
   CPU's idle thread halts until then, the event timer set for the next
   deadline: an idle machine, not a hot one. */
void thread_block_finish(void) {
    if (!initialized) return;
    while (self()->state == THREAD_BLOCKED) yield();
    running_again();
}

void sched_wake_sleepers(uint64_t now) {
    if (!initialized) return;
    uint32_t f = sched_take();
    for (uint32_t i = 0; i < tids.cap; i++) {
        struct thread *t = th((int)i);
        if (!t || !t->sleeping) continue;
        /* On the 64-bit clock, which does not wrap: compared plainly. */
        if (t->wake_ns > now) continue;
        t->sleeping = 0;
        if (t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
            kick(t, 1);
        }
    }
    sched_give(f);
}

/* At any level, that this CPU could run: next_runnable would keep a higher
   current thread, and the timer asks this before halting a CPU that a
   lower one is waiting for. */
int sched_others_ready(void) {
    if (!initialized) return 0;
    int any = 0;
    uint32_t f = sched_take();
    struct cpu *c = cpu_this();
    for (uint32_t i = 0; i < tids.cap && !any; i++) {
        struct thread *t = th((int)i);
        any = t && t != c->cur && runnable_here(t, c);
    }
    sched_give(f);
    return any;
}

uint32_t thread_base(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    uint32_t v = t ? t->base : 0;
    look_done(f);
    return v;
}

uint32_t thread_prio(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    uint32_t v = t ? t->prio : 0;
    look_done(f);
    return v;
}

void thread_set_base(int id, uint32_t level) {
    uint32_t f;
    struct thread *t = look(id, &f);
    if (t) t->base = (uint8_t)level;
    look_done(f);
}

void thread_set_prio(int id, uint32_t level) {
    uint32_t f = sched_take();
    struct thread *t = live(id);
    if (t) {
        t->prio = (uint8_t)level;
        kick(t, 1);
    }
    sched_give(f);
}

struct ipc_call *thread_in_call(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    struct ipc_call *c = t ? t->in_call : 0;
    look_done(f);
    return c;
}

void thread_set_in_call(int id, struct ipc_call *c) {
    uint32_t f;
    struct thread *t = look(id, &f);
    if (t) t->in_call = c;
    look_done(f);
}

void sched_yield_if_outranked(void) {
    if (!initialized) return;
    uint32_t f = sched_take();
    struct cpu *c = cpu_this();
    int go = 0;
    for (uint32_t i = 0; i < tids.cap && !go; i++) {
        struct thread *t = th((int)i);
        go = t && t != c->cur && runnable_here(t, c) && t->prio > self()->prio;
    }
    sched_give(f);
    if (go) yield();
}

void yield(void) {
    if (!initialized) return;
    uint32_t flags = sched_take();
    /* This pick answers any switch asked for (kick, the slice); left
       standing, the request would preempt whoever runs at the next interrupt,
       for nothing, and hide whether that interrupt's own wake asks for one. */
    __atomic_store_n(&cpu_this()->need_resched, 0u, __ATOMIC_RELAXED);
    struct thread *prev = self();
    struct thread *next = next_runnable();
    if (next == prev) { sched_give(flags); return; }   /* only one runnable thread */

    if (prev->state == THREAD_RUNNING) prev->state = THREAD_READY;
    switch_to(next);

    context_switch(&prev->esp, next->esp);
    /* When we resume here later we're `prev` again, now running, and the
       thread that switched to us took sched_lock: ours to give back. */
    sched_unlock_irqs_off();
    sched_reap();   /* reclaim any threads that exited while we were away,
                       still with interrupts off, as their frees always ran */
    irq_restore(flags);
}

/* ---- reaper: reclaim the stacks of exited threads ----
   A thread can't free its own stack (it's running on it), so thread_exit marks
   the slot EXITED and leaves the stack pointers in place. The reaper runs from
   a DIFFERENT thread's context (after a switch) and frees them safely. An
   EXITED thread gives sched_lock up only by its last switch (1.9e2: no
   CPU halts on an exiting thread's stack any more; its idle thread does),
   and the switch clears its on_cpu before the lock goes: a thread still on
   any CPU is skipped all the same, as the reaper's own rule rather than a
   consequence of the handoff. Each is taken under the lock (REAPING, so no
   second reaper takes it too) and freed after it, its resources' locks
   ranking before sched's.

   On every CPU since 1.9e3: freeing a thread can end its process, whose
   handles close through the file system layer, and that layer is locked
   now (the storage lock, sysfile's files_lock). Until then an AP's exited
   threads waited for the boot CPU's next switch or wake. */
static uint32_t reaped_on[CPU_MAX];   /* sched_reaped_on */

uint32_t sched_reaped_on(unsigned index) { return index < CPU_MAX ? reaped_on[index] : 0; }

static void sched_reap(void) {
    for (;;) {
        struct thread *dead = 0;
        uint32_t f = sched_take();
        for (uint32_t i = 1; i < tids.cap && !dead; i++) {
            struct thread *t = th((int)i);
            if (t && t->state == THREAD_EXITED && !t->on_cpu) {
                t->state = THREAD_REAPING;
                dead = t;
            }
        }
        sched_give(f);
        if (!dead) return;
        /* Counted first: the free gives the id back before it is done, and
           whoever sees the count drop sees this too. */
        __atomic_fetch_add(&reaped_on[cpu_this()->index], 1u, __ATOMIC_RELEASE);
        thread_free(dead);   /* its id is reusable */
    }
}

void thread_exit(void) {
    irq_save();   /* never restored here: the next thread resumes with its own flags */
    /* Calls it received stop lifting it, before its id can be reused. */
    ipc_thread_exit(current);
    struct thread *me = self();
    /* Its handles first, then its process's: a waiter on the process finds
       every thread of it ended. */
    if (me->obj) kthread_end(me->obj, me->self_exit ? THR_EXITED : THR_WITH_PROCESS, me->exit_code,
                             cpu_ns_of(me), me->name);
    /* The last one out ends its process now and wakes the waiters: one may be
       the only thread left to run, and it cannot wait for the reaper. The
       last of a process's threads to end by SYS_THREAD_EXIT ends it with its
       code, one before it leaves the rest running: decided where they are
       counted (proc_thread_stopped), since two ending at once on two CPUs
       each saw the other still running here. */
    if (me->proc) proc_thread_stopped(me->proc, me->self_exit ? &me->exit_code : 0);
    /* Its locks were all taken and given back above; sched_lock now, given
       back by the thread switched to (its flags here, off, are no one's). */
    sched_take();
    self()->state = THREAD_EXITED;
    /* Its time stays with its process, which outlives it (thread_proc_cpu_ns
       stops counting an EXITED thread under this same lock). */
    if (me->proc) me->proc->cpu_done_ns += cpu_ns_of(me);
    /* stack is freed later by sched_reap, running in another thread's context
       (we can't free the stack we're still standing on). */

    /* Never itself: an EXITED thread is not runnable, so with every other
       thread blocked or asleep the pick is this CPU's idle thread (1.9e2).
       It used to idle on this stack with the lock given back, which a
       second CPU could not tell from a thread switched out. */
    struct thread *next = next_runnable();
    switch_to(next);
    uintptr_t dummy;
    context_switch(&dummy, next->esp);   /* save into dummy (discarded) */
    /* never reached */
    for (;;) { }
}

int sched_active_count(void) {
    int c = 0;
    uint32_t f = sched_take();
    for (uint32_t i = 0; i < tids.cap; i++) {
        struct thread *t = th((int)i);
        if (t && (t->state == THREAD_READY || t->state == THREAD_RUNNING)) c++;
    }
    sched_give(f);
    return c;
}

/* ---- preemption (Checkpoint 2) ---- */
/* Preemption is on or off for every CPU at once; the slice, the switch
   asked for and the switch under way are each CPU's own (struct cpu,
   1.9e2). The boot CPU's slice runs from the event timer as before; an
   AP's from its own local timer, set when a thread is switched to and
   stopped when its idle thread is (slice_begin). */
static volatile int      preempt_on = 0;
static volatile uint64_t slice_ns = 10 * TIMER_NS_PER_TICK;   /* between switches */

void sched_preempt_enable(uint32_t quantum_ticks) {
    slice_ns = (uint64_t)(quantum_ticks ? quantum_ticks : 1) * TIMER_NS_PER_TICK;
    cpu_boot.slice_end = timer_now_ns() + slice_ns;
    preempt_on = 1;
    timer_deadline_hint(cpu_boot.slice_end);
    /* A thread already running on an AP went through slice_begin with
       preemption off: its timer is stopped and nothing would end its slice.
       Each AP arms its own (sched_cpu_kicked). A switch there either sees
       preempt_on or is followed by this interrupt. smp_call would not do: only
       an idle CPU runs a call. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (unsigned i = 1; i < cpu_count(); i++) smp_kick(i);
}

void sched_preempt_disable(void) {
    preempt_on = 0;
}

/* An AP's slice, from the switch to `next` (sched_lock held): its own timer
   for the end, or stopped for its idle thread and with preemption off. The
   boot CPU's goes on as the event timer has it. */
static void slice_begin(struct cpu *c, const struct thread *next, uint64_t now) {
    if (c == &cpu_boot) return;
    if (next->idle || !preempt_on) { c->slice_end = 0; timer_local_stop(); return; }
    c->slice_end = now + slice_ns;
    timer_local_arm(c->slice_end);
}

/* An AP's own timer (idt.c, its interrupt branch): the slice is over, or it
   came early and is set again for the rest. Touches only this CPU's struct
   while its idle thread runs: an idle CPU's interrupt path reads nothing
   that may have moved (arch/smp.h). */
void sched_cpu_tick(void) {
    struct cpu *c = cpu_this();
    if (c->cur == c->idle_thread) return;
    if (!preempt_on) { c->slice_end = 0; return; }      /* its timer stops here */
    uint64_t now = timer_now_ns();
    if (now < c->slice_end) { timer_local_arm(c->slice_end); return; }
    c->slice_end = now + slice_ns;
    if (!c->in_switch) __atomic_store_n(&c->need_resched, 1u, __ATOMIC_RELAXED);
    timer_local_arm(c->slice_end);
}

/* An AP's IPI: if a thread is running here with no slice, because preemption
   came on after its switch, it has one from now. slice_end is in the future
   only while this CPU's timer is running for it. */
void sched_cpu_kicked(void) {
    struct cpu *c = cpu_this();
    if (c->cur == c->idle_thread || !preempt_on) return;
    uint64_t now = timer_now_ns();
    if (now < c->slice_end) return;
    c->slice_end = now + slice_ns;
    timer_local_arm(c->slice_end);
}

/* called from the timer IRQ (BEFORE the EOI). Flags that a preemptive switch is
   due once the slice has run out; the actual switch is deferred to
   sched_preempt_point(), which the IRQ handler calls AFTER the EOI. Deferring
   past the EOI keeps the controller delivering further interrupts so
   preemption continues. (need_resched is above, with outrank_check: a wake
   of a higher thread asks for the same switch.) */
void sched_tick(uint64_t now) {
    if (!preempt_on || !initialized) return;
    if (now < cpu_boot.slice_end) return;
    /* A new slice even mid-switch, so the end is never left in the past for
       the event timer to be set for again at once. */
    cpu_boot.slice_end = now + slice_ns;
    if (cpu_boot.in_switch) return;        /* don't preempt mid-switch */
    /* the actual switch happens after EOI (sched_preempt_point) */
    __atomic_store_n(&cpu_boot.need_resched, 1u, __ATOMIC_RELAXED);
}

void sched_slice_restart(uint64_t now) {
    cpu_boot.slice_end = now + slice_ns;
}

uint64_t sched_next_due(int running) {
    uint64_t d = TIMER_NEVER;
    if (!initialized) return d;
    if (running && preempt_on) d = cpu_boot.slice_end;
    uint32_t f = sched_take();
    for (uint32_t i = 0; i < tids.cap; i++) {
        struct thread *t = th((int)i);
        if (t && t->sleeping && t->wake_ns < d) d = t->wake_ns;
    }
    sched_give(f);
    return d;
}

/* called from the IRQ handler AFTER pic_send_eoi. Performs the deferred
   preemptive switch if one is due. Doing the switch AFTER the EOI is essential:
   switching before would leave the PIC's in-service bit set, so no further
   timer ticks would arrive and preemption would stall after one switch. */
void sched_preempt_point(void) {
    struct cpu *c = cpu_this();
    if (!__atomic_load_n(&c->need_resched, __ATOMIC_RELAXED)) return;
    /* An idle thread is never switched from inside an interrupt: its loop
       picks once the halt the interrupt ended returns (idle_loop), and an
       idle CPU's interrupt path touches no thread record (sched_cpu_tick). */
    if (c->cur == c->idle_thread) return;
    __atomic_store_n(&c->need_resched, 0u, __ATOMIC_RELAXED);
    if (!preempt_on || !initialized || c->in_switch) return;

    uint32_t f = sched_take();
    struct thread *prev = self();
    struct thread *next = next_runnable();
    if (next == prev) { sched_give(f); return; }

    c->in_switch = 1;
    if (prev->state == THREAD_RUNNING) prev->state = THREAD_READY;
    switch_to(next);
    c->in_switch = 0;
    context_switch(&prev->esp, next->esp);
    /* resumed as `prev` later, the lock handed over by whoever switched here */
    sched_give(f);
    sched_reap();   /* reclaim threads that exited while we were away */
}

int thread_current_id(void) { return current; }

struct proc *thread_proc(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    struct proc *p = t ? t->proc : 0;
    look_done(f);
    return p;
}

int thread_set_affinity(int id, uint32_t mask) {
    uint32_t online = cpu_count() >= 32 ? 0xFFFFFFFFu : (1u << cpu_count()) - 1u;
    mask &= online;
    if (!mask) return -1;
    uint32_t f = sched_take();
    struct thread *t = live(id);
    int rc = -1;
    if (t && !t->idle) {
        t->affinity = mask;
        rc = 0;
        /* On a CPU it may no longer use: that CPU picks again at its next
           preemption point. Ready: a CPU it may now use may be idle. */
        if (t->on_cpu && !(mask & (1u << (t->on_cpu - 1)))) resched(cpu_at(t->on_cpu - 1));
        else kick(t, 1);
    }
    sched_give(f);
    return rc;
}

uint32_t thread_affinity(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    uint32_t m = t ? t->affinity : 0;
    look_done(f);
    return m;
}

uint32_t thread_onto_boot_cpu(void) {
    int me = thread_current_id();
    uint32_t was = thread_affinity(me);
    if (was == THREAD_ON_BOOT_CPU) return was;
    thread_set_affinity(me, THREAD_ON_BOOT_CPU);
    /* The CPU picks again only at a preemption point, and a syscall's body
       has none (interrupts off): give it one until it is the boot CPU. */
    while (cpu_this()->index != 0) yield();
    return was;
}

void thread_affinity_back(uint32_t was) {
    if (was != THREAD_ON_BOOT_CPU) thread_set_affinity(thread_current_id(), was);
}

int thread_cpu(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    int c = t ? (int)t->on_cpu - 1 : -1;
    look_done(f);
    return c;
}

int thread_pid(int id) {
    struct proc *p = thread_proc(id);
    return p ? p->pid : -1;
}

int thread_current_pid(void) {
    return initialized ? thread_pid(current) : 0;
}

const char *thread_name(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    const char *n = t ? t->name : 0;
    look_done(f);
    return n;
}

void thread_set_tls(int id, uint64_t base) {
    uint32_t f = sched_take();
    struct thread *t = id == current ? self() : live(id);
    if (t) {
        t->tls_base = base;
        if (id == current) arch_tls_base_set(base);
    }
    sched_give(f);
}

uint64_t thread_tls(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    uint64_t v = t ? t->tls_base : 0;
    look_done(f);
    return v;
}

void thread_set_own_name(const char *name) {
    struct thread *t = self();
    uint32_t i = 0;
    for (; i + 1u < THREAD_NAME_MAX && name[i]; i++) t->own_name[i] = name[i];
    t->own_name[i] = 0;
    t->name = t->own_name;
}

/* Its slice so far too while it is on a CPU, this one or another (1.9e2). */
static uint64_t cpu_ns_of(const struct thread *t) {
    uint64_t ns = t->cpu_ns;
    if (t->on_cpu) {
        uint64_t now = timer_now_ns();
        if (now > t->run_since) ns += now - t->run_since;
    }
    return ns;
}

uint64_t thread_cpu_ns(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    uint64_t ns = t ? cpu_ns_of(t) : 0;
    look_done(f);
    return ns;
}

uint64_t thread_proc_cpu_ns(const struct proc *p) {
    uint64_t ns = 0;
    uint32_t f = sched_take();
    for (uint32_t i = 0; i < tids.cap; i++) {
        const struct thread *t = th((int)i);
        if (t && t->proc == p && t->state != THREAD_UNUSED && t->state != THREAD_EXITED) ns += cpu_ns_of(t);
    }
    ns += p->cpu_done_ns;
    sched_give(f);
    return ns;
}

/* Into `out`, THREAD_NAME_MAX bytes: t's name, cut short and NUL-terminated. */
static void name_into(const struct thread *t, char *out) {
    uint32_t i = 0;
    const char *n = t && t->name ? t->name : "";
    for (; i + 1u < THREAD_NAME_MAX && n[i]; i++) out[i] = n[i];
    while (i < THREAD_NAME_MAX) out[i++] = 0;
}

void thread_name_copy(int id, char *name) {
    uint32_t f;
    name_into(look(id, &f), name);
    look_done(f);
}

/* is thread `id` a live (non-exited, allocated) thread? Used for stale-lock
   detection: an advisory lock owned by a dead pid is ignorable. */
int thread_is_alive(int id) {
    uint32_t f;
    struct thread *t = look(id, &f);
    enum thread_state st = t ? t->state : THREAD_UNUSED;
    look_done(f);
    return (st == THREAD_READY || st == THREAD_RUNNING);
}

void thread_mark_user(int id) {
    struct proc *p = thread_proc(id);
    if (p) p->is_user = 1;
}

uintptr_t *thread_current_usave(void) {
    return &self()->u_saved_esp;   /* [0]=esp, [1]=flags (contiguous) */
}

void thread_set_kstack_top(int id, uintptr_t top) {
    uint32_t f;
    struct thread *t = look(id, &f);
    if (t) t->kstack_top = top;
    look_done(f);
}

uintptr_t thread_current_kstack_top(void) {
    return self()->kstack_top;
}

/* Allocate a dedicated esp0 (ring-0 entry) stack for a ring-3 process, separate
   from its trampoline stack. When the process is in ring 3 and takes a syscall
   or is preempted by the timer, the CPU switches to THIS stack - so the
   interrupt frame never collides with the trampoline's saved frame on the main
   kernel stack. Returns 0 on success, -1 on alloc failure. */
int thread_alloc_kstack(int id) {
    /* Any live thread: thread 0 (the executive via cxex_exec) DOES need an
       esp0: it runs ring-3 code + makes syscalls while apps coexist. */
    uint32_t f;
    int there = look(id, &f) != 0;
    look_done(f);
    if (!there) return -1;
    /* The stack outside the lock (kstack's ranks after sched's, but a
       mapping's shootdown waits on other CPUs); stored under it. */
    uintptr_t k;
    uintptr_t top = kstack_alloc(stack_bytes, id, &k);
    if (!top) return -1;
    struct thread *t = look(id, &f);
    if (t) {
        t->kstack_base = k;
        t->kstack_top  = top - 16;   /* 16-byte slack at top */
    }
    look_done(f);
    if (!t) { kstack_free(k); return -1; }
    return 0;
}

uint32_t thread_current_cwd(void) {
    if (!initialized) return 0;            /* bare kernel boot context = root */
    return self()->proc->cwd;
}

void thread_set_cwd(int id, uint32_t entry_id) {
    struct proc *p = thread_proc(id);
    if (p) p->cwd = entry_id;
}

uint32_t thread_current_uid(void) {
    if (!initialized) return UID_SYSTEM;   /* bare kernel boot context = SYSTEM */
    return self()->proc->uid;
}

uint32_t thread_current_caps(void) {
    if (!initialized) return 0;            /* bare kernel boot context: ring 0, caps N/A */
    return self()->proc->caps;
}

void thread_set_caps(int id, uint32_t caps) {
    struct proc *p = thread_proc(id);
    if (p) p->caps = caps;
}

struct htab *thread_htab(int id) {
    struct proc *p = thread_proc(id);
    return p ? &p->handles : 0;
}

int32_t thread_broker(int id) {
    struct proc *p = thread_proc(id);
    return p ? p->broker : -1;
}

void thread_set_broker(int id, int32_t h) {
    struct proc *p = thread_proc(id);
    if (p) p->broker = h;
}

void thread_set_uid(int id, uint32_t uid) {
    struct proc *p = thread_proc(id);
    if (!p) return;
    /* Invariant: a user process can never become UID 0 (SYSTEM). If this thread
       is a user process, reject any attempt to set it to SYSTEM - users are
       always UID >= 1. (SYSTEM kernel threads may carry UID 0 legitimately.) */
    if (uid == UID_SYSTEM && p->is_user) return;
    p->uid = uid;
}
#if CXK_TEST_HOOKS
/* 1.9f: sched_lock itself, taken and given by the counted path, so the
   contention count's test holds the one lock the count is for. */
uint32_t sched_lock_test_take(void)       { return sched_take(); }
void     sched_lock_test_give(uint32_t f) { sched_give(f); }
#endif
