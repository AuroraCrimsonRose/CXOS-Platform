// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/proc.c */
/* Aurora Tejeda / CATX Systems */
/* Processes: see proc.h. */
#include "proc.h"
#include "uid.h"
#include "memman/ocache.h"
#include "cpu/int/idt.h"   /* kpanic */

static struct ocache proc_cache;
static struct proc  *by_pid[PROC_PID_MAX];

static void proc_destroy(struct kobj *o) {
    struct proc *p = (struct proc *)o;
    if (p->pid >= 0 && p->pid < PROC_PID_MAX && by_pid[p->pid] == p) by_pid[p->pid] = 0;
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
}

struct proc *proc_create(const struct proc *parent) {
    int pid = -1;
    for (int i = 0; i < PROC_PID_MAX; i++) if (!by_pid[i]) { pid = i; break; }
    if (pid < 0) return 0;

    struct proc *p = ocache_alloc(&proc_cache);   /* zeroed */
    if (!p) return 0;
    if (kobj_init(&p->hdr, KOBJ_PROCESS) != 0) { ocache_free(&proc_cache, p); return 0; }
    p->pid    = pid;
    p->uid    = parent ? parent->uid : UID_SYSTEM;
    p->cwd    = parent ? parent->cwd : 0;
    p->broker = -1;
    htab_init(&p->handles);
    by_pid[pid] = p;
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
    }
    kobj_unref(&p->hdr);
}

void proc_put(struct proc *p) {
    if (p) kobj_unref(&p->hdr);
}

struct proc *proc_by_pid(int pid) {
    if (pid < 0 || pid >= PROC_PID_MAX) return 0;
    return by_pid[pid];
}

uint32_t proc_live(void) { return kobj_live(KOBJ_PROCESS); }
