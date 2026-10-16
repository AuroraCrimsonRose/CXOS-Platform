// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/sched.c */
/* Aurora Tejeda / CATX Systems */
/* Cooperative kernel-thread scheduler (Checkpoint 1). */

#include "sched.h"
#include "heap.h"
#include "kstack.h"
#include "gdt.h"
#include "uid.h"
#include "timer.h"   /* timer_ticks for the sleep queue */
#include "object/idtab.h"
#include "memman/ocache.h"
#include "cpu/int/idt.h"   /* kpanic */

extern void context_switch(uint32_t *old_esp, uint32_t new_esp);

static void sched_reap(void);   /* forward decl: used by yield, defined below */
static uint32_t kernel_pd_phys = 0;   /* CR3 of the shared kernel space (captured at init) */

/* Every thread is a record from its own cache, named by an id from `tids`.
   The id is the index the old fixed array had: small, lowest free first,
   0 for the boot context. The table grows as threads are made, so the limit
   is configuration (sched_configure) and not the size of an array. */
static struct ocache thread_cache;
static struct idtab  tids;
static int current = 0;       /* id of the running thread */

static inline struct thread *th(int id) { return (struct thread *)idtab_get(&tids, id); }
static inline struct thread *self(void) { return th(current); }

/* A live thread (allocated, exited-unreaped included), or NULL. */
static inline struct thread *live(int id) {
    struct thread *t = th(id);
    return (t && t->state != THREAD_UNUSED) ? t : 0;
}
static int initialized = 0;

/* Interrupts off across a switch. yield() and thread_exit() used to run with
   them on, and a timer tick landing after `current = next` but before
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
static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile ("sti" : : : "memory");
}

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

/* a freshly-created thread's stack is hand-crafted so the first switch INTO it
   lands at thread_launch, which calls the entry function. The layout must match
   what context_switch's restore sequence pops: [ebp][edi][esi][ebx][eflags]
   then `ret` pops the return address. We make that return address thread_launch
   and stash the entry fn where thread_launch can find it. */

/* trampoline: the first thing a new thread runs. calls its entry, then exits. */
static void thread_launch(void) {
    void (*entry)(void) = self()->entry;
    if (entry) entry();
    thread_exit();
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
    th(0)->stack_base = 0;   /* uses the existing kernel stack */
    /* the boot/kernel context is the machine (User 0): proc_create with no
       parent gives exactly that */
    th(0)->proc  = proc_create(0);
    if (!th(0)->proc) return;   /* initialized stays 0: nothing schedules */
    proc_thread_in(th(0)->proc);
    proc_put(th(0)->proc);      /* the thread's reference is the one that counts */
    current = 0;
    /* capture the kernel space CR3 (active at boot); kernel threads run in it. */
    __asm__ volatile ("mov %%cr3, %0" : "=r"(kernel_pd_phys));
    initialized = 1;
}

static int thread_create_in(const char *name, void (*entry)(void), int held, struct proc *into);

int thread_create(const char *name, void (*entry)(void)) {
    return thread_create_in(name, entry, 0, 0);
}

int thread_create_sibling(int id, const char *name, void (*entry)(void)) {
    struct proc *p = thread_proc(id);
    if (!p) return -1;
    return thread_create_in(name, entry, 0, p);
}

int thread_create_process(const char *name, void (*entry)(void)) {
    int id = thread_create_in(name, entry, 1, 0);
    if (id < 0) return -1;
    if (thread_alloc_kstack(id) != 0) { thread_discard(id); return -1; }
    return id;
}

void thread_start(int id) {
    struct thread *t = th(id);
    if (id > 0 && t && t->state == THREAD_HELD) t->state = THREAD_READY;
}

int thread_set_arg(int id, void *arg, void (*drop)(void *arg)) {
    struct thread *t = th(id);
    if (id <= 0 || !t || t->state != THREAD_HELD) return -1;
    t->arg      = arg;
    t->arg_drop = drop;
    return 0;
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
    /* The last thread of a process to go closes its handles. */
    proc_thread_gone(t->proc);
    t->proc  = 0;
    t->state = THREAD_UNUSED;
    idtab_take(&tids, t->id);
    ocache_free(&thread_cache, t);
}

/* Never ran, so nothing is standing on its stacks: free them here rather than
   leave it to the reaper, which only collects EXITED threads. */
void thread_discard(int id) {
    struct thread *t = th(id);
    if (id <= 0 || !t || t->state != THREAD_HELD) return;
    uint32_t f = irq_save();
    thread_free(t);
    irq_restore(f);
}

static int thread_create_in(const char *name, void (*entry)(void), int held, struct proc *into) {
    if (!initialized) return -1;
    if (threads_allocated() >= thread_limit) return -1;

    /* Zeroed, and UNUSED until the end: the timer interrupt can see it in the
       table from here on, and must not run it half-built. An EXITED thread
       keeps its id until it is reaped, so its stacks are never clobbered. */
    struct thread *t = ocache_alloc(&thread_cache);
    if (!t) return -1;
    uint32_t f = irq_save();
    int slot = idtab_alloc(&tids, t, 1);
    irq_restore(f);
    if (slot < 0) { ocache_free(&thread_cache, t); return -1; }
    t->id = slot;

    /* A guarded stack (memman/kstack.h): running off the bottom faults rather
       than overwriting the heap, which is what a kmalloc'd stack did. */
    /* Its process: the one it joins, or a new one that inherits the
       creator's uid and cwd and nothing else. */
    struct proc *p = into ? into : proc_create(self()->proc);
    uint32_t stack = 0;
    uint32_t stack_top = p ? kstack_alloc(stack_bytes, slot, &stack) : 0;
    if (!stack_top) {
        if (p && !into) proc_put(p);
        f = irq_save();
        idtab_take(&tids, slot);
        irq_restore(f);
        ocache_free(&thread_cache, t);
        return -1;
    }
    proc_thread_in(p);
    if (!into) proc_put(p);   /* the new thread's reference keeps it */

    /* build the initial stack frame (top-down). The new stack must look exactly
       like a thread that was switched out by context_switch. context_switch
       saves (in this push order): pushfd, ebx, esi, edi, ebp - so from the
       saved esp UPWARD the layout is:
         [esp+0]=ebp [+4]=edi [+8]=esi [+12]=ebx [+16]=eflags [+20]=return addr
       It then restores with: pop ebp; pop edi; pop esi; pop ebx; popfd; ret.
       So we push (high->low addr): return-addr, eflags, ebx, esi, edi, ebp,
       leaving esp pointing at the ebp slot. */
    uint32_t *sp = (uint32_t *)stack_top;
    *(--sp) = (uint32_t)thread_launch;   /* [+20] ret target */
    *(--sp) = 0x202;                     /* [+16] eflags: IF set, reserved bit 1 */
    *(--sp) = 0;                         /* [+12] ebx */
    *(--sp) = 0;                         /* [+8]  esi */
    *(--sp) = 0;                         /* [+4]  edi */
    *(--sp) = 0;                         /* [+0]  ebp  <- esp points here */

    t->esp        = (uint32_t)sp;
    t->stack_base = stack;
    t->name       = name;
    t->proc       = p;
    t->exit_code  = 0;
    t->user_stack_base = 0;
    t->u_saved_esp   = 0;
    t->u_saved_flags = 0;
    /* kstack_top 0: a kernel-only thread, which never enters ring 3 and so runs
       on the dedicated gdt_init stack. A thread that WILL enter ring 3 is made
       with thread_create_process, which gives it its own esp0 before it can be
       scheduled (PROCESS_MODEL.md §8). */
    t->kstack_top  = 0;
    t->kstack_base = 0;
    t->entry       = entry;
    t->arg         = 0;
    t->arg_drop    = 0;
    /* Last: from here the timer may run it. */
    __atomic_store_n(&t->state, held ? THREAD_HELD : THREAD_READY, __ATOMIC_RELEASE);
    return slot;
}

/* pick the next READY/RUNNING thread after `current` (round-robin). */
static int next_runnable(void) {
    int cap = (int)tids.cap;
    for (int n = 1; n <= cap; n++) {
        struct thread *t = th((current + n) % cap);
        if (t && (t->state == THREAD_READY || t->state == THREAD_RUNNING))
            return t->id;
    }
    return current;   /* nobody else: keep running current */
}

/* Point the TSS ring-0 stack at the current thread's esp0 stack, so a
   syscall/interrupt arriving while THIS process is in ring 3 lands on its own
   stack, never another process's. Called on every context switch.

   THE esp0 INVARIANT (PROCESS_MODEL.md §8): after every switch, tss.esp0 is
   the running thread's kstack_top, or the dedicated gdt_init stack when it has
   none. A kernel-only thread used to leave tss.esp0 untouched - still pointing
   at the previous process's esp0 stack, despite this comment saying otherwise.
   Harmless only while no kernel-only thread enters ring 3, which nothing
   checked: one that did would have pushed its frames onto a live process's
   saved interrupt frame. The self-test "esp0 follows the running thread"
   holds it. */
static void update_tss_esp0(void) {
    uint32_t top = self()->kstack_top;
    tss_set_kernel_stack(top ? top : tss_default_kernel_stack());
}

/* Load the current thread's address space into CR3. Threads with pd_phys 0
   (kernel threads) run in the shared kernel space. Called on every switch,
   right after update_tss_esp0(). The kernel half is mapped in every space,
   so this is safe for kernel threads regardless of which space is loaded. */
static void update_address_space(void) {
    uint32_t mine = self()->proc ? self()->proc->pd_phys : 0;
    uint32_t pd = mine ? mine : kernel_pd_phys;
    if (pd) __asm__ volatile ("mov %0, %%cr3" : : "r"(pd) : "memory");
}

void thread_set_space(int id, uint32_t pd_phys) {
    struct proc *p = thread_proc(id);
    if (p) p->pd_phys = pd_phys;
}

/* Block the current thread (e.g. waiting on an IPC reply) and switch away.
   Returns once thread_unblock() has made it runnable and it is scheduled
   again. Callers must ensure another thread is runnable (the IPC rendezvous
   always unblocks its counterpart before blocking). */
void thread_block(void) {
    if (!initialized) return;
    self()->state = THREAD_BLOCKED;
    yield();   /* yield leaves a BLOCKED thread alone; picks another */
}

void thread_wait(void) {
    if (!initialized) return;
    self()->state = THREAD_BLOCKED;
    for (;;) {
        yield();
        if (self()->state != THREAD_BLOCKED) break;  /* woken */
        /* yield found nothing else runnable and stayed: wait for an interrupt
           (a timer tick that wakes a sleeping lock holder) instead of spinning */
        __asm__ volatile ("sti; hlt");
    }
    self()->state = THREAD_RUNNING;
}

void thread_unblock(int id) {
    struct thread *t = th(id);
    if (t && t->state == THREAD_BLOCKED) t->state = THREAD_READY;
}

/* ---- timed sleep ----
 *
 * A sleeping thread leaves the run queue the same way an IPC waiter does, so
 * it costs nothing but its memory. The timer interrupt puts it back.
 */
uint32_t thread_sleep_ms(uint32_t ms) {
    if (!initialized || ms == 0) { yield(); return timer_ticks(); }

    /* All three at once, with the timer held off. Set one at a time, a tick
       landing between `sleeping` and `state` found a thread already due but
       still RUNNING: it cleared `sleeping` and left the state alone, the
       thread then blocked itself, and nothing would ever wake it. A 1 ms
       sleep is due at the very next tick, so the window was real. */
    uint32_t f = irq_save();
    self()->wake_tick = timer_ticks() + ms;
    self()->sleeping  = 1;
    self()->state     = THREAD_BLOCKED;
    irq_restore(f);

    for (;;) {
        yield();
        if (!self()->sleeping) break;                /* the timer woke us */
        if (self()->state != THREAD_BLOCKED) break;  /* something else did */

        /* Still blocked and still here, which means yield found nothing else
           to run and declined to switch. We are the only thread awake, so wait
           for the interrupt that will advance the clock instead of spinning on
           it - the timer is at most one tick away. Halting rather than
           spinning is the difference between an idle machine and a hot one. */
        __asm__ volatile ("sti; hlt");
    }

    self()->sleeping = 0;
    self()->state    = THREAD_RUNNING;
    return timer_ticks();
}

void sched_wake_sleepers(void) {
    if (!initialized) return;
    uint32_t now = timer_ticks();
    for (uint32_t i = 0; i < tids.cap; i++) {
        struct thread *t = th((int)i);
        if (!t || !t->sleeping) continue;
        /* Wrap-safe: a plain `now >= wake_tick` would stop waking anything for
           49 days the first time the counter wrapped. */
        if (!timer_tick_after(now, t->wake_tick)) continue;
        t->sleeping = 0;
        if (t->state == THREAD_BLOCKED) t->state = THREAD_READY;
    }
}

void yield(void) {
    if (!initialized) return;
    uint32_t flags = irq_save();
    int prev = current;
    int next = next_runnable();
    if (next == prev) { irq_restore(flags); return; }   /* only one runnable thread */

    if (th(prev)->state == THREAD_RUNNING) th(prev)->state = THREAD_READY;
    th(next)->state = THREAD_RUNNING;
    current = next;
    update_tss_esp0();
    update_address_space();

    context_switch(&th(prev)->esp, th(next)->esp);
    /* when we resume here later, we're `prev` again, now running */
    sched_reap();   /* reclaim any threads that exited while we were away -
                       still with interrupts off, so two reapers never race */
    irq_restore(flags);
}

/* ---- reaper: reclaim the stacks of exited threads ----
   A thread can't free its own stack (it's running on it), so thread_exit marks
   the slot EXITED and leaves the stack pointers in place. The reaper runs from
   a DIFFERENT thread's context (after a switch) and frees them safely. */
static void sched_reap(void) {
    for (uint32_t i = 1; i < tids.cap; i++) {
        struct thread *t = th((int)i);
        if (t && t->state == THREAD_EXITED && (int)i != current)
            thread_free(t);   /* its id is reusable */
    }
}

void thread_exit(void) {
    irq_save();   /* never restored here: the next thread resumes with its own flags */
    self()->state = THREAD_EXITED;
    /* stack is freed later by sched_reap, running in another thread's context
       (we can't free the stack we're still standing on). */

    /* Nothing else runnable - every other thread blocked or asleep - is not a
       reason to switch to ourselves: our saved esp is from the last time we
       were switched out, and resuming it ran stale context on the stack we
       are leaving. Idle on this stack until an interrupt makes a thread
       runnable (a sleeper's timer, a keystroke). It stays ours until another
       thread reaps it, which cannot happen before we switch away. */
    int next = next_runnable();
    while (next == current) {
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
        next = next_runnable();
    }
    th(next)->state = THREAD_RUNNING;
    current = next;
    update_tss_esp0();
    update_address_space();
    uint32_t dummy;
    context_switch(&dummy, th(next)->esp);   /* save into dummy (discarded) */
    /* never reached */
}

int sched_active_count(void) {
    int c = 0;
    for (uint32_t i = 0; i < tids.cap; i++) {
        struct thread *t = th((int)i);
        if (t && (t->state == THREAD_READY || t->state == THREAD_RUNNING)) c++;
    }
    return c;
}

/* ---- preemption (Checkpoint 2) ---- */
static volatile int      preempt_on = 0;
static volatile uint32_t preempt_quantum = 10;   /* ticks between switches */
static volatile uint32_t preempt_counter = 0;
static volatile int      in_switch = 0;          /* reentrancy guard */

void sched_preempt_enable(uint32_t quantum_ticks) {
    preempt_quantum = quantum_ticks ? quantum_ticks : 1;
    preempt_counter = 0;
    preempt_on = 1;
}

void sched_preempt_disable(void) {
    preempt_on = 0;
}

/* called from the timer IRQ (BEFORE the EOI). Flags that a preemptive switch is
   due every `quantum` ticks; the actual switch is deferred to
   sched_preempt_point(), which the IRQ handler calls AFTER the EOI. Deferring
   past the EOI keeps the PIC delivering further ticks so preemption continues. */
static volatile int need_resched = 0;

void sched_tick(void) {
    if (!preempt_on || !initialized) return;
    if (in_switch) return;                 /* don't preempt mid-switch */
    if (++preempt_counter < preempt_quantum) return;
    preempt_counter = 0;
    need_resched = 1;   /* the actual switch happens after EOI (sched_preempt_point) */
}

/* called from the IRQ handler AFTER pic_send_eoi. Performs the deferred
   preemptive switch if one is due. Doing the switch AFTER the EOI is essential:
   switching before would leave the PIC's in-service bit set, so no further
   timer ticks would arrive and preemption would stall after one switch. */
void sched_preempt_point(void) {
    if (!need_resched) return;
    need_resched = 0;
    if (!preempt_on || !initialized || in_switch) return;

    int prev = current;
    int next = next_runnable();
    if (next == prev) return;

    in_switch = 1;
    if (th(prev)->state == THREAD_RUNNING) th(prev)->state = THREAD_READY;
    th(next)->state = THREAD_RUNNING;
    current = next;
    update_tss_esp0();
    update_address_space();
    in_switch = 0;
    context_switch(&th(prev)->esp, th(next)->esp);
    /* resumed as `prev` later */
    sched_reap();   /* reclaim threads that exited while we were away */
}

int thread_current_id(void) { return current; }

struct proc *thread_proc(int id) {
    struct thread *t = live(id);
    return t ? t->proc : 0;
}

int thread_pid(int id) {
    struct proc *p = thread_proc(id);
    return p ? p->pid : -1;
}

int thread_current_pid(void) {
    return initialized ? thread_pid(current) : 0;
}

const char *thread_name(int id) {
    struct thread *t = live(id);
    return t ? t->name : 0;
}

/* is thread `id` a live (non-exited, allocated) thread? Used for stale-lock
   detection: an advisory lock owned by a dead pid is ignorable. */
int thread_is_alive(int id) {
    struct thread *t = th(id);
    if (!t) return 0;
    enum thread_state st = t->state;
    return (st == THREAD_READY || st == THREAD_RUNNING);
}

void thread_mark_user(int id) {
    struct proc *p = thread_proc(id);
    if (p) p->is_user = 1;
}

uint32_t *thread_current_usave(void) {
    return &self()->u_saved_esp;   /* [0]=esp, [1]=flags (contiguous) */
}

void thread_set_kstack_top(int id, uint32_t top) {
    struct thread *t = live(id);
    if (t) t->kstack_top = top;
}

uint32_t thread_current_kstack_top(void) {
    return self()->kstack_top;
}

/* Allocate a dedicated esp0 (ring-0 entry) stack for a ring-3 process, separate
   from its trampoline stack. When the process is in ring 3 and takes a syscall
   or is preempted by the timer, the CPU switches to THIS stack - so the
   interrupt frame never collides with the trampoline's saved frame on the main
   kernel stack. Returns 0 on success, -1 on alloc failure. */
int thread_alloc_kstack(int id) {
    if (!live(id)) return -1;   /* thread 0 (the executive via
                                                     cxex_exec) DOES need an esp0:
                                                     it runs ring-3 code + makes
                                                     syscalls while apps coexist. */
    uint32_t k;
    uint32_t top = kstack_alloc(stack_bytes, id, &k);
    if (!top) return -1;
    th(id)->kstack_base = k;
    th(id)->kstack_top  = top - 16;   /* 16-byte slack at top */
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