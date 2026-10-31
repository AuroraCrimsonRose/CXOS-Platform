// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_races.c */
/* Aurora Tejeda / CATX Systems */
/* The races, at the kernel's race points - see ktest_races.h. */

#include <stdint.h>
#include "config.h"
#include "ktest_races.h"
#include "diag/testhook.h"
#include "usermode.h"
#include "systable.h"
#include "sched.h"
#include "proc.h"
#include "job.h"
#include "timer.h"
#include "vmregion.h"
#include "futex.h"
#include "logging.h"
#include "object/kobj.h"
#include "object/kmem.h"

#define MS 1000000ull

/* The reason first: the line `cxk test sabotage` reads as the step. */
static int rc_report(const char *why) {
    if (why) klog("KTEST", SEV_FAIL, why);
    return why == 0;
}

/* Wait until the threads and processes a test made are gone and reaped, on
   every path out of it (ktest_hooks.c's hk_settle). */
static int rc_settle(uint32_t threads, uint32_t procs) {
    uint64_t end = timer_now_ns() + 3000 * MS;
    while (sched_thread_count() > threads || proc_live() > procs) {
        if (timer_now_ns() >= end) return 0;
        yield();
    }
    return 1;
}

static void rc_set(uint64_t c[6], uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    c[0] = nr; c[1] = a1; c[2] = a2; c[3] = a3; c[4] = 0; c[5] = 0;
}

/* SYS_FUTEX_WAIT third, after two SYS_THREAD_IDs, which take nothing. */
static void rc_wait_calls(struct um_call3 *c, uint64_t word, uint32_t expected, uint64_t deadline) {
    rc_set(c->c[0], SYS_THREAD_ID, 0, 0, 0);
    rc_set(c->c[1], SYS_THREAD_ID, 0, 0, 0);
#if SYSCALL_PAIRS
    rc_set(c->c[2], SYS_FUTEX_WAIT, word, expected, (uint32_t)deadline);
    c->c[2][4] = deadline >> 32;
#else
    rc_set(c->c[2], SYS_FUTEX_WAIT, word, expected, deadline);
#endif
}

/* The wait, from ring 3 on this thread; USERMODE_FAULTED if it faulted. */
static int rc_wait(uint64_t word, uint32_t expected, uint64_t deadline, void *data, uint32_t len) {
    struct um_call3 c;
    rc_wait_calls(&c, word, expected, deadline);
    return usermode_run_calls(&c, data, len, 0, 0);
}

/* One page, read and write: anonymous, or of `m`, at `hint` if not 0. */
static int rc_map(int pid, struct vm_map_args *a, struct kmem *m, uint64_t hint) {
    uint8_t *b = (uint8_t *)a;
    for (uint32_t i = 0; i < sizeof *a; i++) b[i] = 0;
    a->object = MOBJ_ANON;
    a->length = 4096;
    a->prot   = MPROT_READ | MPROT_WRITE;
    if (hint) { a->flags = MMAP_HINT; a->addr = hint; }
    int rc = vm_map_object(pid, a, m, HRIGHT_MAP | HRIGHT_MAP_WRITE);
    if (rc == E_OK && hint && a->addr != hint) { vm_unmap(pid, a->addr, a->granted); return -1; }
    return rc;
}

/* ---- a word unmapped between its fault-in and its read ---- */

#define UN_EXPECT 0x5A5A5A5Au   /* not what a fresh page holds: E_SHOULD_WAIT, not a wait */

static int      un_pid, un_rc;
static uint64_t un_addr, un_len;

static void un_unmap(void *ctx) { (void)ctx; un_rc = vm_unmap(un_pid, un_addr, un_len); }

static const char *unmapped_steps(int pid) {
    struct vm_map_args a;
    /* Warm, unarmed: the page tables the mapping needs are made here, not
       counted as a leak below; and the word answers E_SHOULD_WAIT at once. */
    if (rc_map(pid, &a, 0, 0) != E_OK) return "futex unmapped: setup map";
    int rc = rc_wait(a.addr, UN_EXPECT, timer_now_ns() + 50 * MS, 0, 0);
    vm_unmap(pid, a.addr, a.granted);
    if (rc != E_SHOULD_WAIT) return "futex unmapped: unarmed not E_SHOULD_WAIT";

    uint32_t w0 = futex_waiting();
    struct live_counts before, after;
    live_take(&before, pid, 0);
    if (rc_map(pid, &a, 0, 0) != E_OK) return "futex unmapped: setup map";
    un_pid = pid; un_addr = a.addr; un_len = a.granted; un_rc = 1;
    if (race_arm(RP_FUTEX_READ, thread_current_id(), 1, RACE_CALL, un_unmap, 0) != 0) {
        vm_unmap(pid, a.addr, a.granted);
        return "futex unmapped: setup arm";
    }
    /* With the read's failure ignored, the wait answers at once or at the
       deadline, never blocks for good. */
    rc = rc_wait(a.addr, UN_EXPECT, timer_now_ns() + 50 * MS, 0, 0);
    uint32_t arrived = race_arrivals(RP_FUTEX_READ);
    race_release(RP_FUTEX_READ);
    if (!arrived) { vm_unmap(pid, a.addr, a.granted); return "futex unmapped: point not reached"; }
    if (un_rc != E_OK)          return "futex unmapped: the unmap failed";
    if (rc != E_BAD_ADDRESS)    return "futex unmapped: not E_BAD_ADDRESS";
    if (futex_waiting() != w0)  return "futex unmapped: left a waiter";
    live_take(&after, pid, 0);
    if (live_diff(&before, &after, 1)) return "futex unmapped: left something behind";
    return 0;
}

int ktest_futex_unmapped(void) {
    /* A kernel thread's process has no address-space account (as the
       census, ktest_sweeps.c). */
    int pid = thread_current_pid();
    vm_proc_init(pid, vm_default_quota(), -1);
    const char *why = unmapped_steps(pid);
    race_release(RP_FUTEX_READ);
    vm_proc_reset(pid);
    return rc_report(why);
}

/* ---- a wake as the deadline comes ---- */

#define DL_WORD  0x100u   /* the word, in the data page */
#define DL_VALUE 7u

static struct { uint64_t deadline; int late, n; } dl;

/* At the point, the block ended by the deadline: a wake now still finds
   the waiter queued. */
static void dl_wake(void *ctx) {
    (void)ctx;
    dl.late = timer_now_ns() >= dl.deadline;
    dl.n    = futex_wake(USERMODE_DATA_VIRT + DL_WORD, 1);
}

static const char *deadline_steps(void) {
    uint8_t data[DL_WORD + 4];
    for (uint32_t i = 0; i < sizeof data; i++) data[i] = 0;
    *(uint32_t *)&data[DL_WORD] = DL_VALUE;
    uint32_t w0 = futex_waiting();

    /* unarmed, the wait runs out */
    int rc = rc_wait(USERMODE_DATA_VIRT + DL_WORD, DL_VALUE, timer_now_ns() + 20 * MS, data, sizeof data);
    if (rc != E_TIMED_OUT) return "futex deadline: unarmed not E_TIMED_OUT";

    dl.deadline = timer_now_ns() + 20 * MS;
    dl.late = dl.n = -1;
    if (race_arm(RP_FUTEX_WOKEN, thread_current_id(), 1, RACE_CALL, dl_wake, 0) != 0)
        return "futex deadline: setup arm";
    rc = rc_wait(USERMODE_DATA_VIRT + DL_WORD, DL_VALUE, dl.deadline, data, sizeof data);
    uint32_t arrived = race_arrivals(RP_FUTEX_WOKEN);
    race_release(RP_FUTEX_WOKEN);
    if (!arrived)              return "futex deadline: point not reached";
    if (dl.late != 1)          return "futex deadline: woken before the deadline";
    if (dl.n != 1)             return "futex deadline: the wake found no waiter";
    if (rc != E_OK)            return "futex deadline: woken waiter not E_OK";
    if (futex_waiting() != w0) return "futex deadline: left a waiter";
    return 0;
}

int ktest_futex_deadline(void) {
    const char *why = deadline_steps();
    race_release(RP_FUTEX_WOKEN);
    return rc_report(why);
}

/* ---- one word, two processes ----
   The helper runs in the kernel's address space, as every ring-3 test
   helper does, so the two processes' mappings are kept apart by address:
   the helper's at VM_MMAP_BASE, this thread's process's above it. */

#define SH_MINE  (VM_MMAP_BASE + 0x100000u)
#define SH_OTHER (VM_MMAP_BASE + 0x200000u)

static const char *shared_steps(int pid, struct kmem *m, struct kmem *other,
                                struct proc **helper, int *tid) {
    struct vm_map_args mine, oth, its;
    if (rc_map(pid, &mine, m, SH_MINE) != E_OK)      return "futex shared: setup map";
    if (rc_map(pid, &oth, other, SH_OTHER) != E_OK) {
        vm_unmap(pid, mine.addr, mine.granted);
        return "futex shared: setup map";
    }
    const char *why = 0;
    uint32_t w0 = futex_waiting();

    /* the helper waits on the word through its own mapping, at offset 0 */
    struct um_call3 c;
    rc_wait_calls(&c, VM_MMAP_BASE, 0, timer_now_ns() + 2000 * MS);
    *helper = usermode_calls_process(&c, tid);
    if (!*helper) { why = "futex shared: setup helper"; goto out; }
    vm_proc_init((*helper)->pid, vm_default_quota(), -1);
    if (rc_map((*helper)->pid, &its, m, 0) != E_OK || its.addr != VM_MMAP_BASE) {
        thread_discard(*tid);
        why = "futex shared: setup helper map";
        goto out;
    }
    thread_start(*tid);

    uint64_t end = timer_now_ns() + 1500 * MS;
    while (futex_waiting() != w0 + 1 && timer_now_ns() < end) yield();
    if (futex_waiting() != w0 + 1) { why = "futex shared: the helper never waited"; goto out; }

    /* another object at the same offset is another word; the same object
       through this process's mapping is the helper's */
    if (futex_wake(oth.addr, 1) != 0)  { why = "futex shared: another object's word woke it"; goto out; }
    if (futex_wake(mine.addr, 1) != 1) { why = "futex shared: no waiter through the other mapping"; goto out; }

    end = timer_now_ns() + 1500 * MS;
    while ((*helper)->ended == PROC_RUNNING && timer_now_ns() < end) yield();
    if ((*helper)->ended != PROC_EXITED || (*helper)->end_code != E_OK)
        why = "futex shared: the helper's wait not E_OK";

out:
    vm_unmap(pid, mine.addr, mine.granted);
    vm_unmap(pid, oth.addr, oth.granted);
    return why;
}

/* A page mapped at `at` and touched, then unmapped: the page tables there
   are made before the snapshot, not counted as a leak after it. */
static void sh_warm(int pid, uint64_t at) {
    struct vm_map_args a;
    if (rc_map(pid, &a, 0, at) != E_OK) return;
    vm_fault(pid, a.addr, 1, 0);
    vm_unmap(pid, a.addr, a.granted);
}

int ktest_futex_shared(void) {
    int pid = thread_current_pid();
    uint32_t threads0 = sched_thread_count(), procs0 = proc_live();
    struct live_counts before, after;
    vm_proc_init(pid, vm_default_quota(), -1);
    sh_warm(pid, VM_MMAP_BASE);
    sh_warm(pid, SH_MINE);
    sh_warm(pid, SH_OTHER);
    live_take(&before, pid, 0);

    int rc;
    struct kmem *m = kmem_create(1, job_root(), &rc);
    struct kmem *other = m ? kmem_create(1, job_root(), &rc) : 0;
    struct proc *helper = 0;
    int tid = -1;
    const char *why = other ? shared_steps(pid, m, other, &helper, &tid) : "futex shared: setup objects";
    if (helper) proc_put(helper);
    if (other) kobj_unref(&other->hdr);
    if (m) kobj_unref(&m->hdr);
    if (!rc_settle(threads0, procs0) && !why) why = "futex shared: the helper never went";
    live_take(&after, pid, 0);
    if (!why && live_diff(&before, &after, 1)) why = "futex shared: left something behind";
    vm_proc_reset(pid);
    return rc_report(why);
}
