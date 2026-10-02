/* /kernel/cpu/sched.c */
/* Aurora Tejeda / CATX Systems */
/* Cooperative kernel-thread scheduler (Checkpoint 1). */

#include "sched.h"
#include "handle.h"
#include "heap.h"
#include "kstack.h"
#include "gdt.h"
#include "uid.h"
#include "timer.h"   /* timer_ticks for the sleep queue */

extern void context_switch(uint32_t *old_esp, uint32_t new_esp);

static void sched_reap(void);   /* forward decl: used by yield, defined below */
static uint32_t kernel_pd_phys = 0;   /* CR3 of the shared kernel space (captured at init) */

static struct thread threads[MAX_THREADS];
static int current = 0;       /* index of the running thread */
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

/* Slots in use, EXITED ones included: an exited thread still holds its stacks
   until the reaper runs. Counting them keeps the limit honest, and because a
   new thread takes the lowest free slot, it also keeps every pid below the
   limit. */
static uint32_t threads_allocated(void) {
    uint32_t n = 0;
    for (int i = 0; i < MAX_THREADS; i++) if (threads[i].state != THREAD_UNUSED) n++;
    return n;
}

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
    /* capture the kernel space CR3 (active at boot); kernel threads run in it. */
    __asm__ volatile ("mov %%cr3, %0" : "=r"(kernel_pd_phys));
    initialized = 1;
}

static int thread_create_in(const char *name, void (*entry)(void), int held);

int thread_create(const char *name, void (*entry)(void)) {
    return thread_create_in(name, entry, 0);
}

int thread_create_process(const char *name, void (*entry)(void)) {
    int id = thread_create_in(name, entry, 1);
    if (id < 0) return -1;
    if (thread_alloc_kstack(id) != 0) { thread_discard(id); return -1; }
    return id;
}

void thread_start(int id) {
    if (id > 0 && id < MAX_THREADS && threads[id].state == THREAD_HELD)
        threads[id].state = THREAD_READY;
}

/* Never ran, so nothing is standing on its stacks: free them here rather than
   leave it to the reaper, which only collects EXITED threads. */
void thread_discard(int id) {
    if (id <= 0 || id >= MAX_THREADS || threads[id].state != THREAD_HELD) return;
    if (threads[id].stack_base)  { kstack_free(threads[id].stack_base);  threads[id].stack_base = 0; }
    if (threads[id].kstack_base) { kstack_free(threads[id].kstack_base); threads[id].kstack_base = 0; }
    threads[id].kstack_top = 0;
    handle_release_all(threads[id].handles, CXK_MAX_HANDLES);
    threads[id].state = THREAD_UNUSED;
}

static int thread_create_in(const char *name, void (*entry)(void), int held) {
    if (!initialized) return -1;
    if (threads_allocated() >= thread_limit) return -1;
    int slot = -1;
    for (int i = 1; i < MAX_THREADS; i++) {
        /* only reuse fully-reaped slots; an EXITED slot still has a stack
           pending free by the reaper, so don't clobber it here. */
        if (threads[i].state == THREAD_UNUSED) {
            slot = i; break;
        }
    }
    if (slot < 0) return -1;

    /* A guarded stack (memman/kstack.h): running off the bottom faults rather
       than overwriting the heap, which is what a kmalloc'd stack did. */
    uint32_t stack;
    uint32_t stack_top = kstack_alloc(stack_bytes, slot, &stack);
    if (!stack_top) return -1;

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

    threads[slot].esp        = (uint32_t)sp;
    threads[slot].stack_base = stack;
    threads[slot].state      = held ? THREAD_HELD : THREAD_READY;
    threads[slot].name       = name;
    threads[slot].is_user    = 0;
    threads[slot].exit_code  = 0;
    threads[slot].uid        = threads[current].uid;   /* inherit creator UID */
    threads[slot].cwd        = threads[current].cwd;   /* inherit creator cwd */
    threads[slot].caps       = 0;                      /* no authority by default (apps) */
    for (int hi = 0; hi < CXK_MAX_HANDLES; hi++) threads[slot].handles[hi].type = HANDLE_NONE;
    threads[slot].user_stack_base = 0;
    threads[slot].u_saved_esp   = 0;
    threads[slot].u_saved_flags = 0;
    /* kstack_top stays 0 for now: cooperative ring-3 uses the single dedicated
       TSS stack (set in gdt_init), which is correct when only one process is in
       ring 3 at a time. Per-process esp0 is wired in step 2b (ring-3 preemption). */
    threads[slot].kstack_top  = 0;
    threads[slot].pd_phys     = 0;     /* kernel space until assigned (spawn) */
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

/* Load the current thread's address space into CR3. Threads with pd_phys 0
   (kernel threads) run in the shared kernel space. Called on every switch,
   right after update_tss_esp0(). The kernel half is mapped in every space,
   so this is safe for kernel threads regardless of which space is loaded. */
static void update_address_space(void) {
    uint32_t pd = threads[current].pd_phys ? threads[current].pd_phys : kernel_pd_phys;
    if (pd) __asm__ volatile ("mov %0, %%cr3" : : "r"(pd) : "memory");
}

void thread_set_space(int id, uint32_t pd_phys) {
    if (id >= 0 && id < MAX_THREADS) threads[id].pd_phys = pd_phys;
}

/* Block the current thread (e.g. waiting on an IPC reply) and switch away.
   Returns once thread_unblock() has made it runnable and it is scheduled
   again. Callers must ensure another thread is runnable (the IPC rendezvous
   always unblocks its counterpart before blocking). */
void thread_block(void) {
    if (!initialized) return;
    threads[current].state = THREAD_BLOCKED;
    yield();   /* yield leaves a BLOCKED thread alone; picks another */
}

void thread_unblock(int id) {
    if (id >= 0 && id < MAX_THREADS && threads[id].state == THREAD_BLOCKED)
        threads[id].state = THREAD_READY;
}

/* ---- timed sleep ----
 *
 * A sleeping thread leaves the run queue the same way an IPC waiter does, so
 * it costs nothing but its memory. The timer interrupt puts it back.
 */
uint32_t thread_sleep_ms(uint32_t ms) {
    if (!initialized || ms == 0) { yield(); return timer_ticks(); }

    threads[current].wake_tick = timer_ticks() + ms;
    threads[current].sleeping  = 1;
    threads[current].state     = THREAD_BLOCKED;

    for (;;) {
        yield();
        if (!threads[current].sleeping) break;                /* the timer woke us */
        if (threads[current].state != THREAD_BLOCKED) break;  /* something else did */

        /* Still blocked and still here, which means yield found nothing else
           to run and declined to switch. We are the only thread awake, so wait
           for the interrupt that will advance the clock instead of spinning on
           it - the timer is at most one tick away. Halting rather than
           spinning is the difference between an idle machine and a hot one. */
        __asm__ volatile ("sti; hlt");
    }

    threads[current].sleeping = 0;
    threads[current].state    = THREAD_RUNNING;
    return timer_ticks();
}

void sched_wake_sleepers(void) {
    if (!initialized) return;
    uint32_t now = timer_ticks();
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!threads[i].sleeping) continue;
        /* Signed difference, so this still works the tick the counter wraps.
           A plain `now >= wake_tick` would stop waking anything for 49 days
           the first time it happened. */
        if ((int32_t)(now - threads[i].wake_tick) < 0) continue;
        threads[i].sleeping = 0;
        if (threads[i].state == THREAD_BLOCKED) threads[i].state = THREAD_READY;
    }
}

void yield(void) {
    if (!initialized) return;
    uint32_t flags = irq_save();
    int prev = current;
    int next = next_runnable();
    if (next == prev) { irq_restore(flags); return; }   /* only one runnable thread */

    if (threads[prev].state == THREAD_RUNNING) threads[prev].state = THREAD_READY;
    threads[next].state = THREAD_RUNNING;
    current = next;
    update_tss_esp0();
    update_address_space();

    context_switch(&threads[prev].esp, threads[next].esp);
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
    for (int i = 1; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_EXITED && i != current) {
            if (threads[i].stack_base) {
                kstack_free(threads[i].stack_base);
                threads[i].stack_base = 0;
            }
            if (threads[i].user_stack_base) {
                kfree((void *)threads[i].user_stack_base);
                threads[i].user_stack_base = 0;
            }
            if (threads[i].kstack_base) {
                kstack_free(threads[i].kstack_base);
                threads[i].kstack_base = 0;
            }
            /* release whatever the handle table still owns - an open file
               belongs to its handle, so a process that exits mid-write must not
               leave the slot held. Endpoints are unaffected (no releaser). */
            handle_release_all(threads[i].handles, CXK_MAX_HANDLES);
            threads[i].state = THREAD_UNUSED;   /* slot reusable */
        }
    }
}

void thread_exit(void) {
    irq_save();   /* never restored here: the next thread resumes with its own flags */
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
    update_address_space();
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
    update_address_space();
    in_switch = 0;
    context_switch(&threads[prev].esp, threads[next].esp);
    /* resumed as `prev` later */
    sched_reap();   /* reclaim threads that exited while we were away */
}

int thread_current_id(void) { return current; }

const char *thread_name(int id) {
    if (id < 0 || id >= MAX_THREADS || threads[id].state == THREAD_UNUSED) return 0;
    return threads[id].name;
}

/* is thread `id` a live (non-exited, allocated) thread? Used for stale-lock
   detection: an advisory lock owned by a dead pid is ignorable. */
int thread_is_alive(int id) {
    if (id < 0 || id >= MAX_THREADS) return 0;
    enum thread_state st = threads[id].state;
    return (st == THREAD_READY || st == THREAD_RUNNING);
}

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
    if (id < 0 || id >= MAX_THREADS) return -1;   /* thread 0 (the executive via
                                                     cxex_exec) DOES need an esp0:
                                                     it runs ring-3 code + makes
                                                     syscalls while apps coexist. */
    uint32_t k;
    uint32_t top = kstack_alloc(stack_bytes, id, &k);
    if (!top) return -1;
    threads[id].kstack_base = k;
    threads[id].kstack_top  = top - 16;   /* 16-byte slack at top */
    return 0;
}

uint32_t thread_current_cwd(void) {
    if (!initialized) return 0;            /* bare kernel boot context = root */
    return threads[current].cwd;
}

void thread_set_cwd(int id, uint32_t entry_id) {
    if (id < 0 || id >= MAX_THREADS) return;
    threads[id].cwd = entry_id;
}

uint32_t thread_current_uid(void) {
    if (!initialized) return UID_SYSTEM;   /* bare kernel boot context = SYSTEM */
    return threads[current].uid;
}

uint32_t thread_current_caps(void) {
    if (!initialized) return 0;            /* bare kernel boot context: ring 0, caps N/A */
    return threads[current].caps;
}

void thread_set_caps(int id, uint32_t caps) {
    if (id >= 0 && id < MAX_THREADS) threads[id].caps = caps;
}

int thread_handle_install(int id, uint8_t type, uint8_t rights, void *object) {
    if (id < 0 || id >= MAX_THREADS) return -1;
    return handle_install(threads[id].handles, CXK_MAX_HANDLES, type, rights, object);
}

struct cap_handle *thread_handle_get(int id, int idx) {
    if (id < 0 || id >= MAX_THREADS) return 0;
    return handle_get(threads[id].handles, CXK_MAX_HANDLES, idx);
}

struct cap_handle *thread_handle_table(int id) {
    if (id < 0 || id >= MAX_THREADS) return 0;
    return threads[id].handles;
}

int thread_handle_close(int id, int idx) {
    if (id < 0 || id >= MAX_THREADS) return -1;
    return handle_close(threads[id].handles, CXK_MAX_HANDLES, idx);
}

void thread_set_uid(int id, uint32_t uid) {
    if (id < 0 || id >= MAX_THREADS) return;
    /* Invariant: a user process can never become UID 0 (SYSTEM). If this thread
       is a user process, reject any attempt to set it to SYSTEM - users are
       always UID >= 1. (SYSTEM kernel threads may carry UID 0 legitimately.) */
    if (uid == UID_SYSTEM && threads[id].is_user) return;
    threads[id].uid = uid;
}