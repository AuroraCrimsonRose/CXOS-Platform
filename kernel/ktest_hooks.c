// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_hooks.c */
/* Aurora Tejeda / CATX Systems */
/* The test hooks' own self-tests, and the first sweeps - see ktest_hooks.h. */

#include <stdint.h>
#include "config.h"
#include "ktest_hooks.h"
#include "diag/testhook.h"
#include "heap.h"
#include "pmm.h"
#include "sched.h"
#include "proc.h"
#include "job.h"
#include "timer.h"
#include "logging.h"
#include "color.h"
#include "vmregion.h"
#include "kstack.h"
#include "memman/ocache.h"
#include "object/kobj.h"
#include "object/kmem.h"
#include "object/kpipe.h"

static struct ocache hk_cache;

static int hk_cache_ready(void) {
    static int ready;
    if (!ready && ocache_init(&hk_cache, "ktest-hooks", 200) == 0) ready = 1;
    return ready;
}

/* Wait for *flag, yielding, at most `ms`. */
static int hk_wait(volatile int *flag, uint32_t ms) {
    uint64_t end = timer_now_ns() + (uint64_t)ms * 1000000u;
    while (!*flag) {
        if (timer_now_ns() >= end) return 0;
        yield();
    }
    return 1;
}

static void hk_pause(uint32_t ms) {
    uint64_t end = timer_now_ns() + (uint64_t)ms * 1000000u;
    while (timer_now_ns() < end) yield();
}

/* Wait until the threads a test made are gone and reaped (the count back to
   `before`), on every path out of it: a helper reaped during the next test
   frees its stack there, and that test sees pages it did not free. */
static int hk_settle(uint32_t before) {
    uint64_t end = timer_now_ns() + 2000ull * 1000000u;
    while (sched_thread_count() > before) {
        if (timer_now_ns() >= end) return 0;
        yield();
    }
    return 1;
}

/* A mapping's arguments: anonymous, one page, read and write. */
static void hk_map_args(struct vm_map_args *a, uint32_t bytes) {
    uint8_t *b = (uint8_t *)a;
    for (uint32_t i = 0; i < sizeof *a; i++) b[i] = 0;
    a->object = MOBJ_ANON;
    a->length = bytes;
    a->prot   = MPROT_READ | MPROT_WRITE;
}

/* ---- failing the Nth allocation ---- */

static volatile int   fn_go, fn_done;
static void *volatile fn_got;
static volatile int   fn_fired;

/* Allocates while the test thread is armed: must not take the failure. */
static void fn_helper(void) {
    hk_wait(&fn_go, 2000);
    fn_got   = kmalloc(100);
    fn_fired = fault_fired();
    fn_done  = 1;
}

static int fault_steps(void) {
    /* kmalloc: the first call fails, the second goes through */
    if (fault_arm(FAULT_KMALLOC, 1) != 0) return 1;
    void *a = kmalloc(100);
    void *b = kmalloc(100);
    uint32_t calls = fault_disarm();
    if (a || !b || !fault_fired() || calls != 2) { kfree(a); kfree(b); return 2; }
    kfree(b);

    /* ... and N counts: the second fails, not the first */
    fault_arm(FAULT_KMALLOC, 2);
    a = kmalloc(100);
    b = kmalloc(100);
    fault_disarm();
    if (!a || b || !fault_fired()) { kfree(a); kfree(b); return 3; }
    kfree(a);

    /* ocache_alloc */
    if (!hk_cache_ready()) return 4;
    fault_arm(FAULT_OCACHE, 1);
    a = ocache_alloc(&hk_cache);
    b = ocache_alloc(&hk_cache);
    fault_disarm();
    if (b) ocache_free(&hk_cache, b);
    if (a || !b) { if (a) ocache_free(&hk_cache, a); return 5; }

    /* pmm: each of the three entry points */
    fault_arm(FAULT_PMM, 1);
    paddr_t p1 = pmm_alloc();
    fault_disarm();
    if (p1) { pmm_free(p1); return 6; }
    fault_arm(FAULT_PMM, 1);
    p1 = pmm_alloc_pages(2);
    fault_disarm();
    if (p1) { pmm_free_pages(p1, 2); return 7; }
    fault_arm(FAULT_PMM, 1);
    p1 = pmm_alloc_pages_low(1);
    fault_disarm();
    if (p1) { pmm_free(p1); return 8; }

    /* kstack_alloc */
    uintptr_t base = 0;
    fault_arm(FAULT_KSTACK, 1);
    uintptr_t top = kstack_alloc(4096, thread_current_id(), &base);
    fault_disarm();
    if (top) { kstack_free(base); return 9; }

    /* vm_map, on a process of the test's own */
    struct proc *scratch = proc_create(0, 0);
    if (!scratch) return 10;
    vm_proc_init(scratch->pid, vm_default_quota(), -1);
    struct vm_map_args m;
    hk_map_args(&m, 4096);
    fault_arm(FAULT_VMMAP, 1);
    int rc1 = vm_map(scratch->pid, &m);
    hk_map_args(&m, 4096);
    int rc2 = vm_map(scratch->pid, &m);
    fault_disarm();
    vm_proc_reset(scratch->pid);
    proc_put(scratch);
    if (rc1 != E_NO_MEMORY || rc2 != E_OK) return 11;

    /* only the armed sites count: a kmalloc under a kstack arming is not one */
    fault_arm(FAULT_KSTACK, 1);
    a = kmalloc(100);
    calls = fault_disarm();
    kfree(a);
    if (!a || calls != 0 || fault_fired()) return 12;

    /* only the arming thread counts */
    fn_go = fn_done = fn_fired = 0;
    fn_got = 0;
    if (thread_create("hook-fault", fn_helper) < 0) return 13;
    fault_arm(FAULT_KMALLOC, 1);
    fn_go = 1;
    int done = hk_wait(&fn_done, 2000);
    a = kmalloc(100);
    fault_disarm();
    kfree(fn_got);
    if (!done || !fn_got || fn_fired) { kfree(a); return 14; }
    if (a) { kfree(a); return 15; }
    return 0;
}

int ktest_fault_nth(void) {
    uint32_t threads0 = sched_thread_count();
    int step = fault_steps();
    fault_disarm();
    fn_go = 1;
    if (!hk_settle(threads0) && !step) step = 16;
    if (step) klog_u32("KTEST", SEV_FAIL, "fail-the-Nth: step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    return step == 0;
}

/* ---- live counts ---- */

static int live_steps(void) {
    struct live_counts a, b;

    /* at rest, nothing moves */
    live_take(&a, -1, 0);
    live_take(&b, -1, 0);
    if (live_diff(&a, &b, 1)) return 1;

    /* a heap block outside the classes */
    void *big = kmalloc(5000);
    if (!big) return 2;
    live_take(&b, -1, 0);
    uint32_t d = live_diff(&a, &b, 0);
    kfree(big);
    if (!d) return 3;
    live_take(&b, -1, 0);
    if (live_diff(&a, &b, 1)) return 4;

    /* a page */
    paddr_t pg = pmm_alloc();
    if (!pg) return 5;
    live_take(&b, -1, 0);
    d = live_diff(&a, &b, 0);
    pmm_free(pg);
    if (d != 1 || b.pages_free + 1 != a.pages_free) return 6;

    /* an object of one cache: only that cache's count moves, by one */
    if (!hk_cache_ready()) return 7;
    live_take(&a, -1, 0);
    void *o = ocache_alloc(&hk_cache);
    if (!o) return 8;
    live_take(&b, -1, 0);
    ocache_free(&hk_cache, o);
    int seen = 0;
    for (uint32_t i = 0; i < b.ncache; i++)
        if (b.cache[i].c == &hk_cache && b.cache[i].live == a.cache[i].live + 1) seen = 1;
    if (!seen) return 9;
    live_take(&b, -1, 0);
    if (live_diff(&a, &b, 1)) return 10;

    /* a process, its mapping, and a job's charge */
    live_take(&a, -1, 0);
    struct proc *scratch = proc_create(0, 0);
    if (!scratch) return 11;
    live_take(&b, -1, 0);
    if (b.procs != a.procs + 1 || b.types[KOBJ_PROCESS] != a.types[KOBJ_PROCESS] + 1) {
        proc_put(scratch); return 12;
    }
    int pid = scratch->pid;
    vm_proc_init(pid, vm_default_quota(), -1);
    live_take(&a, pid, 0);
    struct vm_map_args m;
    hk_map_args(&m, 8192);
    int rc = vm_map(pid, &m);
    live_take(&b, pid, 0);
    vm_proc_reset(pid);
    proc_put(scratch);
    if (rc != E_OK || b.mapped != a.mapped + 8192) return 13;

    int jrc;
    struct job *j = job_create(job_root(), 0, 0, 0, &jrc);
    if (!j) return 14;
    live_take(&a, -1, j);
    struct kmem *km = kmem_create(2, j, &jrc);
    live_take(&b, -1, j);
    if (km) kobj_unref(&km->hdr);
    struct live_counts c;
    live_take(&c, -1, j);
    job_put(j);
    if (!km || b.job_mem != a.job_mem + 8192) return 15;
    if (live_diff(&a, &c, 1)) return 16;
    return 0;
}

int ktest_live_counts(void) {
    int step = live_steps();
    if (step) klog_u32("KTEST", SEV_FAIL, "live counts: step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    return step == 0;
}

/* ---- race points ---- */

static volatile int rp_go, rp_passed, rp_calls, rp_decoy_go, rp_decoy_done;

static void rp_call(void *ctx) { (*(volatile int *)ctx)++; }

static void rp_helper(void) {
    hk_wait(&rp_go, 2000);
    race_point(RP_SELFTEST);
    rp_passed = 1;
}

/* Reaches the point the helper is armed at, first: it must pass. */
static void rp_decoy(void) {
    hk_wait(&rp_decoy_go, 2000);
    race_point(RP_SELFTEST);
    rp_decoy_done = 1;
}

static int race_steps(void) {
    /* a call at the second arrival, and only once */
    rp_calls = 0;
    if (race_arm(RP_SELFTEST, thread_current_id(), 2, RACE_CALL, rp_call, (void *)&rp_calls) != 0)
        return 1;
    race_point(RP_SELFTEST);
    if (rp_calls != 0 || race_arrivals(RP_SELFTEST) != 1) return 2;
    race_point(RP_SELFTEST);
    if (rp_calls != 1) return 3;
    race_point(RP_SELFTEST);
    if (rp_calls != 1 || race_arrivals(RP_SELFTEST) != 2) return 4;

    /* a park, of the helper only: a decoy passes the same point freely. If
       the decoy were parked instead, release lets it go, so a broken filter
       is a red step here, not a hang. */
    rp_go = rp_passed = rp_decoy_go = rp_decoy_done = 0;
    int h = thread_create("hook-race", rp_helper);
    if (h < 0) return 5;
    int dc = thread_create("hook-decoy", rp_decoy);
    if (dc < 0) { rp_go = 1; return 5; }
    if (race_arm(RP_SELFTEST, h, 1, RACE_PARK, 0, 0) != 0) { rp_go = rp_decoy_go = 1; return 6; }
    rp_decoy_go = 1;
    if (!hk_wait(&rp_decoy_done, 500) || race_arrivals(RP_SELFTEST) != 0) {
        race_release(RP_SELFTEST);
        rp_go = 1;
        return 7;
    }
    rp_go = 1;
    int parked = race_wait_parked(RP_SELFTEST, 2000);
    if (parked != h) { race_release(RP_SELFTEST); return 8; }
    hk_pause(20);
    int early = rp_passed;
    race_release(RP_SELFTEST);
    if (early) return 9;
    if (!hk_wait(&rp_passed, 2000)) return 10;

    /* bad arguments are refused */
    if (race_arm(RP_COUNT, -1, 1, RACE_PARK, 0, 0) == 0) return 11;
    if (race_arm(RP_SELFTEST, -1, 0, RACE_PARK, 0, 0) == 0) return 12;
    if (race_arm(RP_SELFTEST, -1, 1, RACE_CALL, 0, 0) == 0) return 13;
    return 0;
}

int ktest_race_points(void) {
    uint32_t threads0 = sched_thread_count();
    int step = race_steps();
    race_release(RP_SELFTEST);
    rp_go = rp_decoy_go = 1;
    if (!hk_settle(threads0) && !step) step = 14;
    if (step) klog_u32("KTEST", SEV_FAIL, "race points: step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    return step == 0;
}

/* ---- the sweeps ----
   Each operation makes something and undoes it, returning 0, or undoes
   what it made and returns nonzero when a step was refused. The sweep fails
   the 1st, 2nd, ... allocation the operation makes until a run reaches no
   Nth: every run must leave the live counts as they were, and a run whose
   allocation failed must say so. Two warm runs first, so a slab or a page
   table made once and kept is not counted as a leak. */

#define SWEEP_MAX 64

static int sweep_pid = -1;

static int op_pipe(void) {
    int rc;
    struct kpipe *p = kpipe_create(PIPE_SIZE_MIN, job_root(), &rc);
    if (!p) return rc ? rc : -1;
    kobj_unref(&p->rd.hdr);
    kobj_unref(&p->wr.hdr);
    return 0;
}

static int op_mem(void) {
    int rc;
    struct kmem *m = kmem_create(3, job_root(), &rc);
    if (!m) return rc ? rc : -1;
    kmem_page(m, 1);
    kobj_unref(&m->hdr);
    return 0;
}

static int op_job(void) {
    int rc;
    struct job *j = job_create(job_root(), 1u << 20, 4, 0, &rc);
    if (!j) return rc ? rc : -1;
    job_put(j);
    return 0;
}

static int op_proc(void) {
    struct proc *p = proc_create(0, 0);
    if (!p) return -1;
    proc_put(p);
    return 0;
}

static void op_never(void) { }

static int op_thread(void) {
    int id = thread_create_process("sweep", op_never, 0);
    if (id < 0) return id;
    thread_discard(id);
    return 0;
}

static int op_map(void) {
    struct vm_map_args m;
    hk_map_args(&m, 3 * 4096);
    int rc = vm_map(sweep_pid, &m);
    if (rc != E_OK) return rc;
    int touched = vm_fault(sweep_pid, m.addr + 4096, 1, 0);
    if (vm_unmap(sweep_pid, m.addr, m.granted) != E_OK) return -2;
    return touched ? 0 : -3;
}

/* "<name>: N allocations failed in turn", to serial only: a sweep that
   reached none would pass while testing nothing, and this says how deep each
   one went. */
static void sweep_note(const char *name, uint32_t n) {
    char line[96];
    uint32_t i = 0;
    while (*name && i < 60) line[i++] = *name++;
    const char *mid = ": ";
    while (*mid) line[i++] = *mid++;
    char d[10]; int k = 0;
    do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) line[i++] = d[--k];
    const char *tail = " allocations failed in turn";
    while (*tail) line[i++] = *tail++;
    line[i] = 0;
    klog_offscreen("KTEST", SEV_OK, line);
}

static int sweep(const char *name, int (*op)(void), int pid) {
    for (int w = 0; w < 2; w++)
        if (op() != 0) {
            klog("KTEST", SEV_FAIL, name);
            klog_child("refused with no failure armed");
            return 0;
        }

    for (uint32_t n = 1; n <= SWEEP_MAX; n++) {
        struct live_counts before, after;
        live_take(&before, pid, job_root());
        fault_arm(FAULT_ALL, n);
        int rc = op();
        fault_disarm();
        int fired = fault_fired();
        live_take(&after, pid, job_root());

        const char *why = 0;
        if (live_diff(&before, &after, 1)) why = "left something behind";
        else if (fired && rc == 0)         why = "a failed allocation was not reported";
        else if (!fired && rc != 0)        why = "refused with no failure reached";
        if (why) {
            klog("KTEST", SEV_FAIL, name);
            klog_child_u32("failing allocation ", n, LOG_COLOR_VALUE, "");
            klog_child(why);
            return 0;
        }
        if (!fired) {           /* the operation makes fewer than n */
            sweep_note(name, n - 1);
            if (n == 1) {       /* it allocated nothing: the sweep tested nothing */
                klog("KTEST", SEV_FAIL, name);
                klog_child("no allocation was reached");
                return 0;
            }
            return 1;
        }
    }
    klog("KTEST", SEV_FAIL, name);
    klog_child("more allocations than the sweep allows");
    return 0;
}

int ktest_alloc_sweeps(void) {
    int ok = 1;
    ok &= sweep("sweep: pipe",          op_pipe,   -1);
    ok &= sweep("sweep: memory object", op_mem,    -1);
    ok &= sweep("sweep: job",           op_job,    -1);
    ok &= sweep("sweep: process",       op_proc,   -1);
    ok &= sweep("sweep: held thread",   op_thread, -1);

    struct proc *scratch = proc_create(0, 0);
    if (!scratch) return 0;
    sweep_pid = scratch->pid;
    vm_proc_init(sweep_pid, vm_default_quota(), -1);
    ok &= sweep("sweep: mapping",       op_map,    sweep_pid);
    vm_proc_reset(sweep_pid);
    proc_put(scratch);
    sweep_pid = -1;
    return ok;
}
