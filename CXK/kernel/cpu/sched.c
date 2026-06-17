/* /CXK/kernel/cpu/sched.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Cooperative kernel-thread scheduler (Checkpoint 1). */

#include "sched.h"
#include "heap.h"

extern void context_switch(uint32_t *old_esp, uint32_t new_esp);

static void sched_reap(void);   /* forward decl: used by yield, defined below */

static struct thread threads[MAX_THREADS];
static int current = 0;       /* index of the running thread */
static int initialized = 0;

/* a freshly-created thread's stack is hand-crafted so the first switch INTO it
   lands at thread_launch, which calls the entry function. The layout must match
   what context_switch's restore sequence pops: [ebp][edi][esi][ebx][eflags]
   then `ret` pops the return address. We make that return address thread_launch
   and stash the entry fn where thread_launch can find it. */

/* per-thread entry function, looked up by thread_launch via `current` */
static void (*thread_entry[MAX_THREADS])(void);

/* trampoline: the first thing a new thread runs. calls its entry, then exits. */
static void thread_launch(void) {
    void (*entry)(void) = thread_entry[current];
    if (entry) entry();
    thread_exit();
}

void sched_init(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        threads[i].state = THREAD_UNUSED;
        threads[i].id = i;
    }
    /* thread 0 = the currently-running kernel context. Its esp is filled in on
       the first switch away from it (context_switch saves into &threads[0].esp). */
    threads[0].state = THREAD_RUNNING;
    threads[0].name  = "main";
    threads[0].stack_base = 0;   /* uses the existing kernel stack */
    current = 0;
    initialized = 1;
}

int thread_create(const char *name, void (*entry)(void)) {
    if (!initialized) return -1;
    int slot = -1;
    for (int i = 1; i < MAX_THREADS; i++) {
        /* only reuse fully-reaped slots; an EXITED slot still has a stack
           pending free by the reaper, so don't clobber it here. */
        if (threads[i].state == THREAD_UNUSED) {
            slot = i; break;
        }
    }
    if (slot < 0) return -1;

    uint32_t stack = (uint32_t)kmalloc(THREAD_STACK);
    if (!stack) return -1;

    /* build the initial stack frame (top-down). The new stack must look exactly
       like a thread that was switched out by context_switch:
         [ return address = thread_launch ]   <- ret pops this
         [ ebp ] [ edi ] [ esi ] [ ebx ] [ eflags ]  <- pops, in this order
       so esp points at the saved ebp slot. */
    uint32_t *sp = (uint32_t *)(stack + THREAD_STACK);
    *(--sp) = (uint32_t)thread_launch;   /* ret target */
    *(--sp) = 0;                         /* ebp */
    *(--sp) = 0;                         /* edi */
    *(--sp) = 0;                         /* esi */
    *(--sp) = 0;                         /* ebx */
    *(--sp) = 0x202;                     /* eflags: IF set, reserved bit 1 */

    threads[slot].esp        = (uint32_t)sp;
    threads[slot].stack_base = stack;
    threads[slot].state      = THREAD_READY;
    threads[slot].name       = name;
    threads[slot].is_user    = 0;
    threads[slot].exit_code  = 0;
    threads[slot].user_stack_base = 0;
    thread_entry[slot]       = entry;
    return slot;
}

/* pick the next READY/RUNNING thread after `current` (round-robin). */
static int next_runnable(void) {
    for (int n = 1; n <= MAX_THREADS; n++) {
        int i = (current + n) % MAX_THREADS;
        if (threads[i].state == THREAD_READY || threads[i].state == THREAD_RUNNING)
            return i;
    }
    return current;   /* nobody else: keep running current */
}

void yield(void) {
    if (!initialized) return;
    int prev = current;
    int next = next_runnable();
    if (next == prev) return;     /* only one runnable thread */

    if (threads[prev].state == THREAD_RUNNING) threads[prev].state = THREAD_READY;
    threads[next].state = THREAD_RUNNING;
    current = next;

    context_switch(&threads[prev].esp, threads[next].esp);
    /* when we resume here later, we're `prev` again, now running */
    sched_reap();   /* reclaim any threads that exited while we were away */
}

/* ---- reaper: reclaim the stacks of exited threads ----
   A thread can't free its own stack (it's running on it), so thread_exit marks
   the slot EXITED and leaves the stack pointers in place. The reaper runs from
   a DIFFERENT thread's context (after a switch) and frees them safely. */
static void sched_reap(void) {
    for (int i = 1; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_EXITED && i != current) {
            if (threads[i].stack_base) {
                kfree((void *)threads[i].stack_base);
                threads[i].stack_base = 0;
            }
            if (threads[i].user_stack_base) {
                kfree((void *)threads[i].user_stack_base);
                threads[i].user_stack_base = 0;
            }
            threads[i].state = THREAD_UNUSED;   /* slot reusable */
        }
    }
}

void thread_exit(void) {
    threads[current].state = THREAD_EXITED;
    /* stack is freed later by sched_reap, running in another thread's context
       (we can't free the stack we're still standing on). */

    int next = next_runnable();
    if (next == current) {
        /* no one else runnable: fall back to thread 0 if it's alive */
        for (int i = 0; i < MAX_THREADS; i++) {
            if (i != current && (threads[i].state == THREAD_READY ||
                                 threads[i].state == THREAD_RUNNING)) { next = i; break; }
        }
    }
    threads[next].state = THREAD_RUNNING;
    current = next;
    uint32_t dummy;
    context_switch(&dummy, threads[next].esp);   /* save into dummy (discarded) */
    /* never reached */
}

int sched_active_count(void) {
    int c = 0;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].state == THREAD_READY || threads[i].state == THREAD_RUNNING) c++;
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

/* called from the timer IRQ. Performs an involuntary yield every quantum ticks.
   The switch reuses the proven context_switch path: because we're inside the
   timer IRQ, the IRQ stub has already saved the caller-saved registers, and
   context_switch preserves the callee-saved set - so the full state of the
   preempted thread is intact and it resumes exactly where it left off. */
void sched_tick(void) {
    if (!preempt_on || !initialized) return;
    if (in_switch) return;                 /* don't preempt mid-switch */
    if (++preempt_counter < preempt_quantum) return;
    preempt_counter = 0;

    int prev = current;
    int next = next_runnable();
    if (next == prev) return;              /* only one runnable: nothing to do */

    /* mark that a switch is in progress, then clear it right before handing
       off, so the thread we switch TO is immediately preemptible again. The
       counter reset above already prevents an immediate re-fire. */
    in_switch = 1;
    if (threads[prev].state == THREAD_RUNNING) threads[prev].state = THREAD_READY;
    threads[next].state = THREAD_RUNNING;
    current = next;
    in_switch = 0;
    context_switch(&threads[prev].esp, threads[next].esp);
    /* resumed as `prev` later */
    sched_reap();   /* reclaim threads that exited while we were away */
}

int thread_current_id(void) { return current; }

void thread_mark_user(int id) {
    if (id >= 0 && id < MAX_THREADS) threads[id].is_user = 1;
}