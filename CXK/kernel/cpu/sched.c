/* /CXK/kernel/cpu/sched.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Cooperative kernel-thread scheduler (Checkpoint 1). */

#include "sched.h"
#include "heap.h"
#include "gdt.h"
#include "uid.h"

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
    threads[0].uid   = UID_SYSTEM;   /* the boot/kernel context is the machine (User 0) */
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
       like a thread that was switched out by context_switch. context_switch
       saves (in this push order): pushfd, ebx, esi, edi, ebp - so from the
       saved esp UPWARD the layout is:
         [esp+0]=ebp [+4]=edi [+8]=esi [+12]=ebx [+16]=eflags [+20]=return addr
       It then restores with: pop ebp; pop edi; pop esi; pop ebx; popfd; ret.
       So we push (high->low addr): return-addr, eflags, ebx, esi, edi, ebp,
       leaving esp pointing at the ebp slot. */
    uint32_t *sp = (uint32_t *)(stack + THREAD_STACK);
    *(--sp) = (uint32_t)thread_launch;   /* [+20] ret target */
    *(--sp) = 0x202;                     /* [+16] eflags: IF set, reserved bit 1 */
    *(--sp) = 0;                         /* [+12] ebx */
    *(--sp) = 0;                         /* [+8]  esi */
    *(--sp) = 0;                         /* [+4]  edi */
    *(--sp) = 0;                         /* [+0]  ebp  <- esp points here */

    threads[slot].esp        = (uint32_t)sp;
    threads[slot].stack_base = stack;
    threads[slot].state      = THREAD_READY;
    threads[slot].name       = name;
    threads[slot].is_user    = 0;
    threads[slot].exit_code  = 0;
    threads[slot].uid        = threads[current].uid;   /* inherit creator UID */
    threads[slot].user_stack_base = 0;
    threads[slot].u_saved_esp   = 0;
    threads[slot].u_saved_flags = 0;
    /* kstack_top stays 0 for now: cooperative ring-3 uses the single dedicated
       TSS stack (set in gdt_init), which is correct when only one process is in
       ring 3 at a time. Per-process esp0 is wired in step 2b (ring-3 preemption). */
    threads[slot].kstack_top  = 0;
    threads[slot].kstack_base = 0;
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

/* Point the TSS ring-0 stack at the current thread's kernel stack, so a
   syscall/interrupt arriving while THIS process is in ring 3 lands on its own
   kernel stack (not another process's). Kernel-only threads have kstack_top 0
   and keep the default dedicated TSS stack. Called on every context switch. */
static void update_tss_esp0(void) {
    uint32_t top = threads[current].kstack_top;
    if (top) tss_set_kernel_stack(top);
}

void yield(void) {
    if (!initialized) return;
    int prev = current;
    int next = next_runnable();
    if (next == prev) return;     /* only one runnable thread */

    if (threads[prev].state == THREAD_RUNNING) threads[prev].state = THREAD_READY;
    threads[next].state = THREAD_RUNNING;
    current = next;
    update_tss_esp0();

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
            if (threads[i].kstack_base) {
                kfree((void *)threads[i].kstack_base);
                threads[i].kstack_base = 0;
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
    update_tss_esp0();
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
    if (threads[prev].state == THREAD_RUNNING) threads[prev].state = THREAD_READY;
    threads[next].state = THREAD_RUNNING;
    current = next;
    update_tss_esp0();
    in_switch = 0;
    context_switch(&threads[prev].esp, threads[next].esp);
    /* resumed as `prev` later */
    sched_reap();   /* reclaim threads that exited while we were away */
}

int thread_current_id(void) { return current; }

void thread_mark_user(int id) {
    if (id >= 0 && id < MAX_THREADS) threads[id].is_user = 1;
}

uint32_t *thread_current_usave(void) {
    return &threads[current].u_saved_esp;   /* [0]=esp, [1]=flags (contiguous) */
}

void thread_set_kstack_top(int id, uint32_t top) {
    if (id >= 0 && id < MAX_THREADS) threads[id].kstack_top = top;
}

uint32_t thread_current_kstack_top(void) {
    return threads[current].kstack_top;
}

/* Allocate a dedicated esp0 (ring-0 entry) stack for a ring-3 process, separate
   from its trampoline stack. When the process is in ring 3 and takes a syscall
   or is preempted by the timer, the CPU switches to THIS stack - so the
   interrupt frame never collides with the trampoline's saved frame on the main
   kernel stack. Returns 0 on success, -1 on alloc failure. */
int thread_alloc_kstack(int id) {
    if (id <= 0 || id >= MAX_THREADS) return -1;
    uint32_t k = (uint32_t)kmalloc(THREAD_STACK);
    if (!k) return -1;
    threads[id].kstack_base = k;
    threads[id].kstack_top  = k + THREAD_STACK - 16;   /* 16-byte slack at top */
    return 0;
}

uint32_t thread_current_uid(void) {
    if (!initialized) return UID_SYSTEM;   /* bare kernel boot context = SYSTEM */
    return threads[current].uid;
}

void thread_set_uid(int id, uint32_t uid) {
    if (id < 0 || id >= MAX_THREADS) return;
    /* Invariant: a user process can never become UID 0 (SYSTEM). If this thread
       is a user process, reject any attempt to set it to SYSTEM - users are
       always UID >= 1. (SYSTEM kernel threads may carry UID 0 legitimately.) */
    if (uid == UID_SYSTEM && threads[id].is_user) return;
    threads[id].uid = uid;
}