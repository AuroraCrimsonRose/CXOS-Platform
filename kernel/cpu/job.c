// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/job.c */
/* Aurora Tejeda / CATX Systems */
/* Jobs: see job.h. */
#include "job.h"
#include "proc.h"           /* proc_kill_within */
#include "memman/ocache.h"
#include "object/kwait.h"
#include "arch/cpu_ops.h"   /* arch_irq_save */
#include "cxk_abi.h"
#include "panic.h"

static struct ocache job_cache;
static struct job   *root;

/* Interrupts off. Signalled once: killed, and nothing left running in it. */
static void maybe_ended(struct job *j) {
    if (j->killed && j->procs == 0) kobj_signal(&j->hdr, SIGNAL_JOB_ENDED);
}

static void job_destroy(struct kobj *o) {
    struct job *j = (struct job *)o;
    struct job *parent = j->parent;
    if (parent) {
        uint32_t f = arch_irq_save();
        for (struct job **pp = &parent->children; *pp; pp = &(*pp)->sibling)
            if (*pp == j) { *pp = j->sibling; break; }
        arch_irq_restore(f);
    }
    ocache_free(&job_cache, j);
    if (parent) job_put(parent);
}

static const struct kobj_ops job_ops = {
    .name          = "job",
    .rights_valid  = HRIGHT_INSPECT | HRIGHT_KILL | HRIGHT_ASSIGN | HRIGHT_SET_LIMITS,
    .signals_valid = SIGNAL_JOB_ENDED,
    .transfer      = 1,
    .destroy       = job_destroy,
};

void job_sys_init(void) {
    if (ocache_init(&job_cache, "job", sizeof(struct job)) != 0 ||
        kobj_type_register(KOBJ_JOB, &job_ops) != 0)
        kpanic("job_sys_init: no job cache");
    root = ocache_alloc(&job_cache);   /* zeroed: no limits, no parent */
    if (!root || kobj_init(&root->hdr, KOBJ_JOB) != 0) kpanic("job_sys_init: no root job");
    /* Its creation reference is never dropped. */
}

struct job *job_root(void) { return root; }

struct job *job_create(struct job *parent, uint64_t mem_limit, uint32_t thread_limit,
                       uint32_t prio_limit, int *rc) {
    if (parent->depth + 1u > JOB_DEPTH_MAX) { *rc = E_OUT_OF_RANGE; return 0; }
    struct job *j = ocache_alloc(&job_cache);   /* zeroed */
    if (!j || kobj_init(&j->hdr, KOBJ_JOB) != 0) {
        if (j) ocache_free(&job_cache, j);
        *rc = E_NO_MEMORY;
        return 0;
    }
    j->depth        = parent->depth + 1u;
    j->mem_limit    = mem_limit;
    j->thread_limit = thread_limit;
    j->prio_limit   = prio_limit;
    /* Checked and linked in one step, so a kill cannot land between them and
       miss it. */
    uint32_t f = arch_irq_save();
    if (parent->killed) {
        arch_irq_restore(f);
        job_put(j);   /* no parent yet: destroying it unlinks nothing */
        *rc = E_CANCELED;
        return 0;
    }
    kobj_ref(&parent->hdr);
    j->parent        = parent;
    j->sibling       = parent->children;
    parent->children = j;
    arch_irq_restore(f);
    *rc = E_OK;
    return j;
}

void job_set_limits(struct job *j, uint64_t mem_limit, uint32_t thread_limit, uint32_t prio_limit) {
    uint32_t f = arch_irq_save();
    j->mem_limit    = mem_limit;
    j->thread_limit = thread_limit;
    j->prio_limit   = prio_limit;
    arch_irq_restore(f);
}

uint32_t job_prio_ceiling(const struct job *j) {
    uint32_t c = PRIO_MAX;
    uint32_t f = arch_irq_save();
    for (const struct job *a = j; a; a = a->parent)
        if (a->prio_limit && a->prio_limit < c) c = a->prio_limit;
    arch_irq_restore(f);
    return c;
}

int job_charge_mem(struct job *j, uint64_t bytes) {
    uint32_t f = arch_irq_save();
    for (struct job *a = j; a; a = a->parent)
        if (a->mem_used + bytes < a->mem_used ||
            (a->mem_limit && a->mem_used + bytes > a->mem_limit)) {
            arch_irq_restore(f);
            return E_NO_MEMORY;
        }
    /* Every job has room: only now is any of them charged. */
    for (struct job *a = j; a; a = a->parent) {
        a->mem_used += bytes;
        if (a->mem_used > a->mem_peak) a->mem_peak = a->mem_used;
    }
    arch_irq_restore(f);
    return E_OK;
}

void job_uncharge_mem(struct job *j, uint64_t bytes) {
    uint32_t f = arch_irq_save();
    /* Clamped, as vm_uncharge is: an over-refund would be room nobody gave. */
    for (struct job *a = j; a; a = a->parent)
        a->mem_used = bytes > a->mem_used ? 0 : a->mem_used - bytes;
    arch_irq_restore(f);
}

int job_charge_thread(struct job *j) {
    uint32_t f = arch_irq_save();
    for (struct job *a = j; a; a = a->parent)
        if (a->thread_limit && a->threads >= a->thread_limit) {
            arch_irq_restore(f);
            return E_NO_RESOURCES;
        }
    for (struct job *a = j; a; a = a->parent) a->threads++;
    arch_irq_restore(f);
    return E_OK;
}

void job_uncharge_thread(struct job *j) {
    uint32_t f = arch_irq_save();
    for (struct job *a = j; a; a = a->parent)
        if (a->threads) a->threads--;
    arch_irq_restore(f);
}

void job_proc_in(struct job *j) {
    kobj_ref(&j->hdr);
    uint32_t f = arch_irq_save();
    for (struct job *a = j; a; a = a->parent) a->procs++;
    arch_irq_restore(f);
}

void job_proc_out(struct job *j) {
    uint32_t f = arch_irq_save();
    for (struct job *a = j; a; a = a->parent) {
        if (a->procs) a->procs--;
        maybe_ended(a);
    }
    arch_irq_restore(f);
}

int job_within(const struct job *j, const struct job *top) {
    for (; j; j = j->parent)
        if (j == top) return 1;
    return 0;
}

/* Interrupts off. The tree is at most JOB_DEPTH_MAX deep, so the recursion
   is bounded. */
static void close_below(struct job *j) {
    j->killed = 1;
    for (struct job *c = j->children; c; c = c->sibling) close_below(c);
    maybe_ended(j);
}

void job_kill(struct job *j, int killer) {
    /* Closed first, so a process made from here on is refused its first
       thread; then every one already in it is ended. */
    uint32_t f = arch_irq_save();
    close_below(j);
    arch_irq_restore(f);
    proc_kill_within(j, killer);
}

void job_put(struct job *j) {
    if (j) kobj_unref(&j->hdr);
}

uint32_t job_live(void) { return kobj_live(KOBJ_JOB); }
