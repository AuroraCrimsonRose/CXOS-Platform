// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/proc.c */
/* Aurora Tejeda / CATX Systems */
/* Processes: see proc.h. */
#include "proc.h"
#include "uid.h"
#include "memman/ocache.h"
#include "object/idtab.h"
#include "object/kwait.h"
#include "sched.h"          /* sched_wake_process */
#include "arch/cpu_ops.h"   /* arch_irq_save */
#include "cxk_abi.h"
#include "sync/spinlock.h"
#include "panic.h"
#include "arch/paging.h"
#include "logging.h"
#include "format.h"

static struct ocache proc_cache;
static struct idtab  pids;   /* pid -> process */
/* The reaper frees a process with interrupts off, possibly while a preempted
   proc_create is part way through growing the table. */
static spinlock_t    pids_lock = SPINLOCK_INIT;
static uint32_t      handle_limit = HTAB_LIMIT_DEFAULT;   /* kernel.xkco max_handles */

/* Its own address space, frames, page tables and directory, and the
   bookkeeping that describes them. Never while one of its threads can run in
   it: the last one is gone, or there never was one. */
static void release_space(struct proc *p) {
    paddr_t pd = p->pd_phys;
    if (!pd) return;
    p->pd_phys = 0;
    vm_proc_reset(p->pid);    /* bookkeeping only: the release frees the frames */
    addr_space_release(pd);
}

static void proc_destroy(struct kobj *o) {
    struct proc *p = (struct proc *)o;
    /* Empty already if it ever had a thread (proc_thread_gone). */
    htab_close_all(&p->handles);
    htab_destroy(&p->handles);
    release_space(p);
    uint32_t f = spin_lock(&pids_lock);
    if (idtab_get(&pids, p->pid) == p) idtab_take(&pids, p->pid);
    spin_unlock(&pids_lock, f);
    ocache_free(&proc_cache, p);
}

static const struct kobj_ops proc_ops = {
    .name         = "process",
    .rights_valid  = HRIGHT_INSPECT | HRIGHT_KILL,
    .signals_valid = SIGNAL_PROCESS_ENDED,
    .destroy      = proc_destroy,
};

void proc_sys_init(void) {
    if (ocache_init(&proc_cache, "process", sizeof(struct proc)) != 0 ||
        kobj_type_register(KOBJ_PROCESS, &proc_ops) != 0)
        kpanic("proc_sys_init: no process cache");
    idtab_init(&pids, PROC_PID_MAX);
}

struct proc *proc_create(const struct proc *parent) {
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
    return p;
}

void proc_thread_in(struct proc *p) {
    kobj_ref(&p->hdr);
    p->threads++;
    p->running++;
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
    /* Every way into ring 3 notes its way out; nothing noted, it never got
       there. Signalled last, so a waiter that wakes finds every handle the
       process held already closed. */
    if (p->ended == PROC_RUNNING) p->ended = PROC_NOT_STARTED;
    kobj_signal(&p->hdr, SIGNAL_PROCESS_ENDED);
}

void proc_thread_gone(struct proc *p) {
    /* However the thread ended: SYS_EXIT, a fault, a failed start. Its
       creator used to tear the space down on the way out of the trampoline,
       so every way out that skipped that line leaked it. Freed here, once no
       thread can be standing in it. */
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
   to wake, so it is left as it was. */
void proc_kill(struct proc *p, int killer) {
    uint32_t f = arch_irq_save();
    if (p->ended == PROC_RUNNING) {
        p->ended      = PROC_KILLED;
        p->end_killer = (uint32_t)killer;
    }
    p->killing = 1;
    sched_wake_process(p);
    arch_irq_restore(f);
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
