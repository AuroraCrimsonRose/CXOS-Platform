// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/proc.c */
/* Aurora Tejeda / CATX Systems */
/* Processes: see proc.h. */
#include "proc.h"
#include "uid.h"
#include "memman/ocache.h"
#include "object/idtab.h"
#include "sync/spinlock.h"
#include "panic.h"
#include "memman/addr_space.h"

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
    uint32_t pd = p->pd_phys;
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
    .rights_valid = 0,              /* no process handles yet (plan section 4) */
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
    p->broker = -1;
    htab_init(&p->handles, handle_limit);
    return p;
}

void proc_thread_in(struct proc *p) {
    kobj_ref(&p->hdr);
    p->threads++;
}

void proc_thread_gone(struct proc *p) {
    if (--p->threads == 0) {
        /* Every object belongs to its handles, so a process that ends mid-write
           or with an endpoint open leaves nothing behind. */
        htab_close_all(&p->handles);
        p->broker = -1;
        /* However the thread ended: SYS_EXIT, a fault, a failed start. Its
           creator used to tear the space down on the way out of the
           trampoline, so every way out that skipped that line leaked it. */
        release_space(p);
    }
    kobj_unref(&p->hdr);
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
