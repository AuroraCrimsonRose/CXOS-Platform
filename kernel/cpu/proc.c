// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/proc.c */
/* Aurora Tejeda / CATX Systems */
/* Processes: see proc.h. */
#include "proc.h"
#include "job.h"
#include "uid.h"
#include "memman/ocache.h"
#include "object/idtab.h"
#include "object/kwait.h"
#include "sched.h"          /* sched_wake_process */
#include "ipc.h"            /* ep_post_stop */
#include "timer.h"          /* stop deadlines */
#include "arch/cpu_ops.h"   /* arch_irq_save */
#include "cxk_abi.h"
#include "sync/spinlock.h"
#include "panic.h"
#include "arch/paging.h"
#include "logging.h"
#include "format.h"
#include "object/ktimer.h"   /* ktimer_sys_init */
#include "object/kqueue.h"   /* kqueue_sys_init */
#include "object/knotify.h"  /* knotify_sys_init */

static struct ocache proc_cache;
static struct idtab  pids;   /* pid -> process */
/* The reaper frees a process with interrupts off, possibly while a preempted
   proc_create is part way through growing the table. */
static spinlock_t    pids_lock = SPINLOCK_INIT;
static uint32_t      handle_limit = HTAB_LIMIT_DEFAULT;   /* kernel.xkco max_handles */
/* Processes with a stop deadline armed, linked through stop_next. Changed
   only with interrupts off, since the tick walks it. A process is unlinked
   before its last thread is through (proc_thread_stopped), so the list never
   holds a reference: nothing on it can be freed. */
static struct proc  *stops;

static void stop_unlink(struct proc *p) {
    if (!p->stop_armed) return;
    for (struct proc **pp = &stops; *pp; pp = &(*pp)->stop_next)
        if (*pp == p) { *pp = p->stop_next; break; }
    p->stop_next  = 0;
    p->stop_armed = 0;
}

/* Its own address space, frames, page tables and directory, and the
   bookkeeping that describes them. Never while one of its threads can run in
   it: the last one is gone, or there never was one. The bookkeeping goes
   even for a process in the kernel's space, since it is what gives the job
   back its charge. */
static void release_space(struct proc *p) {
    vm_proc_reset(p->pid);    /* bookkeeping only: the release frees the frames */
    paddr_t pd = p->pd_phys;
    if (!pd) return;
    p->pd_phys = 0;
    addr_space_release(pd);
}

/* Out of its job's count, once: when it ends, or here if it never did. */
static void leave_job(struct proc *p) {
    if (!p->in_job) return;
    p->in_job = 0;
    job_proc_out(p->job);
}

static void proc_destroy(struct kobj *o) {
    struct proc *p = (struct proc *)o;
    /* Empty already if it ever had a thread (proc_thread_gone). */
    htab_close_all(&p->handles);
    htab_destroy(&p->handles);
    release_space(p);
    leave_job(p);
    uint32_t f = spin_lock(&pids_lock);
    if (idtab_get(&pids, p->pid) == p) idtab_take(&pids, p->pid);
    spin_unlock(&pids_lock, f);
    struct job *j = p->job;
    ocache_free(&proc_cache, p);
    job_put(j);
}

static const struct kobj_ops proc_ops = {
    .name         = "process",
    .rights_valid  = HRIGHT_INSPECT | HRIGHT_KILL | HRIGHT_STOP,
    .signals_valid = SIGNAL_PROCESS_ENDED,
    .transfer     = 1,
    .destroy      = proc_destroy,
};

void proc_sys_init(void) {
    job_sys_init();
    ktimer_sys_init();
    kqueue_sys_init();
    knotify_sys_init();
    if (ocache_init(&proc_cache, "process", sizeof(struct proc)) != 0 ||
        kobj_type_register(KOBJ_PROCESS, &proc_ops) != 0)
        kpanic("proc_sys_init: no process cache");
    idtab_init(&pids, PROC_PID_MAX);
}

struct proc *proc_create(const struct proc *parent, struct job *job) {
    struct proc *p = ocache_alloc(&proc_cache);   /* zeroed */
    if (!p) return 0;
    uint32_t f = spin_lock(&pids_lock);
    int pid = idtab_alloc(&pids, p, 0);
    spin_unlock(&pids_lock, f);
    if (pid < 0) { ocache_free(&proc_cache, p); return 0; }
    if (kobj_init(&p->hdr, KOBJ_PROCESS) != 0) {
        f = spin_lock(&pids_lock);
        idtab_take(&pids, pid);
        spin_unlock(&pids_lock, f);
        ocache_free(&proc_cache, p);
        return 0;
    }
    p->pid    = pid;
    p->uid    = parent ? parent->uid : UID_SYSTEM;
    p->cwd    = parent ? parent->cwd : 0;
    p->background = parent ? parent->background : 0;   /* never handed back */
    p->broker = -1;
    htab_init(&p->handles, handle_limit);
    p->job    = job ? job : parent ? parent->job : job_root();
    job_proc_in(p->job);
    p->in_job = 1;
    return p;
}

/* Interrupts off from the check to the count, so a job_kill either finds the
   thread counted (and kills it) or closed the job before the check. */
int proc_thread_in(struct proc *p) {
    uint32_t f = arch_irq_save();
    int rc = p->job->killed ? E_CANCELED : job_charge_thread(p->job);
    if (rc == E_OK) {
        kobj_ref(&p->hdr);
        p->threads++;
        p->running++;
    }
    arch_irq_restore(f);
    return rc;
}

/* On the way out of the last running thread itself, not later in the reaper:
   a waiter may be the only other thread, and the reaper runs only once
   something else is scheduled, so a wait for it waited forever. Interrupts
   off (thread_exit, or thread_free for a thread that never ran). */
void proc_thread_stopped(struct proc *p) {
    if (--p->running != 0) return;
    /* Every object belongs to its handles, so a process that ends mid-write
       or with an endpoint open leaves nothing behind. */
    proc_out_flush(p);
    htab_close_all(&p->handles);
    p->broker = -1;
    /* Nobody is left to ask, or to force. */
    stop_unlink(p);
    if (p->stop_ep) { ep_put(p->stop_ep); p->stop_ep = 0; }
    /* Every way into ring 3 notes its way out; nothing noted, it never got
       there. Signalled last, so a waiter that wakes finds every handle the
       process held already closed. */
    if (p->ended == PROC_RUNNING) p->ended = PROC_NOT_STARTED;
    kobj_signal(&p->hdr, SIGNAL_PROCESS_ENDED);
    /* After its own signal: a job's waiter that wakes finds each process in
       it ended too. */
    leave_job(p);
}

void proc_thread_gone(struct proc *p) {
    /* However the thread ended: SYS_EXIT, a fault, a failed start. Its
       creator used to tear the space down on the way out of the trampoline,
       so every way out that skipped that line leaked it. Freed here, once no
       thread can be standing in it. */
    job_uncharge_thread(p->job);
    if (--p->threads == 0) release_space(p);
    kobj_unref(&p->hdr);
}

void proc_note_exit(struct proc *p, int32_t code) {
    if (!p || p->ended != PROC_RUNNING) return;
    p->ended    = PROC_EXITED;
    p->end_code = code;
}

void proc_note_crash(struct proc *p, uint32_t vector, uint64_t ip, uint64_t addr) {
    if (!p || p->ended != PROC_RUNNING) return;
    p->ended      = PROC_CRASHED;
    p->end_vector = vector;
    p->end_ip     = ip;
    p->end_addr   = addr;
}

/* Interrupts off from the note to the last wake: a thread that checks
   thread_killed and then blocks does both with interrupts off too, so it
   either sees `killing` or is already BLOCKED and gets woken here. One that
   has ended was noted already (proc_thread_stopped) and has no threads left
   to wake, so it is left as it was. A stop's deadline ends a process the
   same way, noted PROC_STOPPED. */
static void proc_force(struct proc *p, uint32_t how, int by) {
    uint32_t f = arch_irq_save();
    if (p->ended == PROC_RUNNING) {
        p->ended      = how;
        p->end_killer = (uint32_t)by;
    }
    p->killing = 1;
    sched_wake_process(p);
    arch_irq_restore(f);
}

void proc_kill(struct proc *p, int killer) {
    proc_force(p, PROC_KILLED, killer);
}

/* Every process not yet ended, not only the running ones: one being made has
   no thread yet, and is noted killed now rather than missed (its first
   thread is refused, the job being closed). */
void proc_kill_within(struct job *top, int killer) {
    uint32_t f = spin_lock(&pids_lock);
    for (int pid = 0; pid < PROC_PID_MAX; pid++) {
        struct proc *p = (struct proc *)idtab_get(&pids, pid);
        if (p && p->in_job && job_within(p->job, top)) proc_force(p, PROC_KILLED, killer);
    }
    spin_unlock(&pids_lock, f);
}

/* Interrupts off throughout: the process's own thread may be replacing its
   stop endpoint, and the tick reads the armed list. */
void proc_stop(struct proc *p, int by, uint32_t ms) {
    uint32_t f = arch_irq_save();
    if (p->running == 0) { arch_irq_restore(f); return; }
    if (!p->stop_ep || ep_post_stop(p->stop_ep, by, ms) != 0) {
        arch_irq_restore(f);
        proc_force(p, PROC_STOPPED, by);
        return;
    }
    uint64_t due = timer_now_ns() + (uint64_t)ms * TIMER_NS_PER_TICK;
    if (!p->stop_armed) {
        p->stop_next  = stops;
        stops         = p;
        p->stop_armed = 1;
        p->stop_due   = due;
        p->stop_by    = by;
    } else if (due < p->stop_due) {
        p->stop_due = due;
        p->stop_by  = by;
    }
    arch_irq_restore(f);
    timer_deadline_hint(due);
}

void proc_set_stop_endpoint(struct proc *p, struct endpoint *ep) {
    if (ep) kobj_ref(&ep->hdr);
    uint32_t f = arch_irq_save();
    struct endpoint *old = p->stop_ep;
    p->stop_ep = ep;
    arch_irq_restore(f);
    if (old) ep_put(old);
}

uint64_t proc_stop_next_due(void) {
    uint64_t d = TIMER_NEVER;
    for (struct proc *p = stops; p; p = p->stop_next)
        if (p->stop_due < d) d = p->stop_due;
    return d;
}

void proc_stop_tick(uint64_t now) {
    struct proc **pp = &stops;
    while (*pp) {
        struct proc *p = *pp;
        if (p->stop_due > now) { pp = &p->stop_next; continue; }
        *pp = p->stop_next;
        p->stop_next  = 0;
        p->stop_armed = 0;
        proc_force(p, PROC_STOPPED, p->stop_by);
    }
}

uint32_t proc_stops_armed(void) {
    uint32_t n = 0;
    uint32_t f = arch_irq_save();
    for (struct proc *p = stops; p; p = p->stop_next) n++;
    arch_irq_restore(f);
    return n;
}

void proc_put(struct proc *p) {
    if (p) kobj_unref(&p->hdr);
}

struct proc *proc_by_pid(int pid) {
    uint32_t f = spin_lock(&pids_lock);
    struct proc *p = (struct proc *)idtab_get(&pids, pid);
    spin_unlock(&pids_lock, f);
    return p;
}

void proc_set_handle_limit(uint32_t n) { handle_limit = n; }
uint32_t proc_handle_limit(void)        { return handle_limit; }

uint32_t proc_live(void) { return kobj_live(KOBJ_PROCESS); }

void proc_out_flush(struct proc *p) {
    if (!p->outlen) return;
    char tag[12] = "pid ";
    tag[4 + fmt_u32(tag + 4, (uint32_t)p->pid)] = 0;
    p->outbuf[p->outlen] = 0;
    p->outlen = 0;
    klog_offscreen(tag, SEV_INFO, p->outbuf);
}

void proc_out_write(struct proc *p, const char *s, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\n') { proc_out_flush(p); continue; }
        if (c == '\r') continue;
        if ((unsigned char)c < ' ' && c != '\t') c = '?';   /* no escapes into the log */
        p->outbuf[p->outlen++] = c;
        if (p->outlen == sizeof p->outbuf - 1) proc_out_flush(p);
    }
}
