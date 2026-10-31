// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/diag/testhook.c */
/* Aurora Tejeda / CATX Systems */
/* The self-tests' hooks - see testhook.h. */

#include "diag/testhook.h"
#include "sched.h"
#include "pmm.h"
#include "heap.h"
#include "memman/kstack.h"
#include "memman/ocache.h"
#include "memman/vmregion.h"
#include "object/kobj.h"
#include "object/htab.h"
#include "proc.h"
#include "job.h"
#include "timer.h"
#include "logging.h"
#include "color.h"
#include "panic.h"

_Static_assert(KOBJ_TYPE_MAX <= 16, "live_counts.types holds every object type");

#if CXK_TEST_HOOKS

volatile uint32_t spin_held;   /* sync/spinlock.h */

/* ---- failing the Nth allocation ----
   Armed and read by one thread, the arming one; the other threads only read
   fault_armed and fault_tid, which change while no call of the arming
   thread is counting. */
volatile int      fault_armed;
static volatile int      fault_tid;
static volatile uint32_t fault_sites, fault_nth, fault_calls;
static volatile int      fault_did;

int fault_hit_slow(uint32_t site) {
    if (!(fault_sites & site) || thread_current_id() != fault_tid) return 0;
    if (++fault_calls != fault_nth) return 0;
    fault_did = 1;
    return 1;
}

int fault_arm(uint32_t sites, uint32_t nth) {
    if (!nth || !(sites & FAULT_ALL)) return -1;
    fault_armed = 0;
    fault_tid   = thread_current_id();
    fault_sites = sites & FAULT_ALL;
    fault_nth   = nth;
    fault_calls = 0;
    fault_did   = 0;
    fault_armed = 1;
    return 0;
}

uint32_t fault_disarm(void) {
    fault_armed = 0;
    return fault_calls;
}

int fault_fired(void) { return fault_did; }

/* ---- race points ---- */

struct race {
    int      tid;            /* -1: any */
    uint32_t nth, arrivals;
    int      act;
    void   (*fn)(void *ctx);
    void    *ctx;
    volatile int parked;     /* the parked thread's id + 1; 0 none */
    volatile int go;         /* race_release: the parked thread goes on */
};

volatile uint32_t race_armed_mask;
static struct race races[RP_COUNT];

void race_point_slow(enum race_point p) {
    if ((unsigned)p >= RP_COUNT) return;
    struct race *r = &races[p];
    int me = thread_current_id();
    if (r->tid >= 0 && r->tid != me) return;
    if (++r->arrivals != r->nth) return;
    /* Once per arming: no other thread can take it after this one. */
    race_armed_mask &= ~(1u << p);
    if (spin_held) kpanic("race point reached holding a spinlock");

    if (r->act == RACE_CALL) { r->fn(r->ctx); return; }
    r->parked = me + 1;
    while (!r->go) yield();
    r->parked = 0;
}

int race_arm(enum race_point p, int tid, uint32_t nth, enum race_action act,
             void (*fn)(void *ctx), void *ctx) {
    if ((unsigned)p >= RP_COUNT || !nth) return -1;
    if (act != RACE_PARK && act != RACE_CALL) return -1;
    if (act == RACE_CALL && !fn) return -1;
    struct race *r = &races[p];
    race_armed_mask &= ~(1u << p);
    r->tid = tid; r->nth = nth; r->arrivals = 0;
    r->act = act; r->fn = fn; r->ctx = ctx;
    r->parked = 0; r->go = 0;
    race_armed_mask |= 1u << p;
    return 0;
}

int race_wait_parked(enum race_point p, uint32_t ms) {
    if ((unsigned)p >= RP_COUNT) return -1;
    uint64_t end = timer_now_ns() + (uint64_t)ms * 1000000u;
    while (!races[p].parked) {
        if (timer_now_ns() >= end) return -1;
        yield();
    }
    return races[p].parked - 1;
}

void race_release(enum race_point p) {
    if ((unsigned)p >= RP_COUNT) return;
    race_armed_mask &= ~(1u << p);
    races[p].go = 1;
}

uint32_t race_arrivals(enum race_point p) {
    return (unsigned)p < RP_COUNT ? races[p].arrivals : 0;
}

#else   /* the hooks compiled out: every request is refused */

int      fault_arm(uint32_t sites, uint32_t nth) { (void)sites; (void)nth; return -1; }
uint32_t fault_disarm(void) { return 0; }
int      fault_fired(void)  { return 0; }
int  race_arm(enum race_point p, int tid, uint32_t nth, enum race_action act,
              void (*fn)(void *ctx), void *ctx) {
    (void)p; (void)tid; (void)nth; (void)act; (void)fn; (void)ctx; return -1;
}
int  race_wait_parked(enum race_point p, uint32_t ms) { (void)p; (void)ms; return -1; }
void race_release(enum race_point p) { (void)p; }
uint32_t race_arrivals(enum race_point p) { (void)p; return 0; }

#endif

/* ---- live counts: no hook of their own, so built either way ---- */

static int cache_add(struct ocache *c, void *ctx) {
    struct live_counts *s = ctx;
    if (s->ncache >= LIVE_CACHES) return 1;
    s->cache[s->ncache].c    = c;
    s->cache[s->ncache].live = ocache_live(c);
    s->ncache++;
    return 0;
}

void live_take(struct live_counts *s, int pid, const struct job *job) {
    uint8_t *b = (uint8_t *)s;
    for (uint32_t i = 0; i < sizeof *s; i++) b[i] = 0;

    s->pages_free     = pmm_free_count();
    s->pages_reserved = pmm_reserved_count();
    s->heap_bytes     = heap_bytes_used();
    s->heap_blocks    = heap_allocs_live();
    s->kstacks        = kstack_live();
    s->threads        = sched_thread_count();
    s->procs          = proc_live();
    s->jobs           = job_live();
    for (uint32_t t = 1; t < KOBJ_TYPE_MAX; t++) s->types[t] = kobj_live((enum kobj_type)t);
    ocache_each(cache_add, s);

    s->pid = pid;
    struct proc *p = pid >= 0 ? proc_by_pid(pid) : 0;
    if (p) s->handles = htab_count(&p->handles);
    struct vm_usage u;
    if (pid >= 0 && vm_info(pid, &u) == 0) s->mapped = u.mapped;

    s->job = job;
    if (job) {
        s->job_mem     = job->mem_used;
        s->job_threads = job->threads;
        s->job_procs   = job->procs;
    }
}

static uint32_t differ(const char *what, uint64_t a, uint64_t b, int log) {
    if (a == b) return 0;
    if (log) {
        klog_u32("LIVE", SEV_WARN, what, (uint32_t)a, LOG_COLOR_VALUE, " before");
        klog_child_u32("after: ", (uint32_t)b, LOG_COLOR_VALUE, "");
    }
    return 1;
}

uint32_t live_diff(const struct live_counts *a, const struct live_counts *b, int log) {
    uint32_t n = 0;
    n += differ("free pages: ",     a->pages_free,     b->pages_free,     log);
    n += differ("reserved pages: ", a->pages_reserved, b->pages_reserved, log);
    n += differ("heap bytes: ",     a->heap_bytes,     b->heap_bytes,     log);
    n += differ("heap blocks: ",    a->heap_blocks,    b->heap_blocks,    log);
    n += differ("kernel stacks: ",  a->kstacks,        b->kstacks,        log);
    n += differ("threads: ",        a->threads,        b->threads,        log);
    n += differ("processes: ",      a->procs,          b->procs,          log);
    n += differ("jobs: ",           a->jobs,           b->jobs,           log);
    for (uint32_t t = 1; t < KOBJ_TYPE_MAX; t++) {
        if (a->types[t] == b->types[t]) continue;
        if (log) klog_u32("LIVE", SEV_WARN, "objects of type ", t, LOG_COLOR_VALUE, ":");
        n += differ("objects: ", a->types[t], b->types[t], log);
    }

    /* The cache list only grows (a cache is never torn down), in one order. */
    for (uint32_t i = 0; i < b->ncache; i++) {
        uint32_t before = i < a->ncache && a->cache[i].c == b->cache[i].c ? a->cache[i].live : 0;
        if (before != b->cache[i].live) {
            if (log) klog("LIVE", SEV_WARN, ocache_name(b->cache[i].c));
            n += differ("cache objects: ", before, b->cache[i].live, log);
        }
    }

    if (a->pid != b->pid || a->job != b->job) return n + 1;
    n += differ("handles: ",      a->handles,     b->handles,     log);
    n += differ("mapped bytes: ", a->mapped,      b->mapped,      log);
    n += differ("job bytes: ",    a->job_mem,     b->job_mem,     log);
    n += differ("job threads: ",  a->job_threads, b->job_threads, log);
    n += differ("job procs: ",    a->job_procs,   b->job_procs,   log);
    return n;
}
