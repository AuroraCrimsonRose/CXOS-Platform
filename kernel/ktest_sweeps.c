// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_sweeps.c */
/* Aurora Tejeda / CATX Systems */
/* The syscall failure sweeps from ring 3 - see ktest_sweeps.h. */

#include <stdint.h>
#include "config.h"
#include "ktest_sweeps.h"
#include "diag/testhook.h"
#include "usermode.h"
#include "systable.h"
#include "sched.h"
#include "proc.h"
#include "vmregion.h"
#include "logging.h"
#include "color.h"
#include "object/htab.h"

static int           sw_me;
static struct htab  *sw_tab;
static struct job   *sw_job;

static void sw_where(void) {
    sw_me  = thread_current_id();
    sw_tab = thread_htab(sw_me);
    sw_job = thread_proc(sw_me)->job;
}

static void sw_live(struct live_counts *s) { live_take(s, thread_current_pid(), sw_job); }

/* The call third, after two SYS_THREAD_IDs, which take nothing: the blob
   exits with what the third returned. */
static int sw_ring3(const uint64_t call[6], void *data, uint32_t len, uint32_t nth, int *faulted) {
    struct um_call3 c;
    for (int i = 0; i < 2; i++) {
        c.c[i][0] = SYS_THREAD_ID;
        for (int k = 1; k < 6; k++) c.c[i][k] = 0;
    }
    for (int k = 0; k < 6; k++) c.c[2][k] = call[k];
    uint32_t f0 = usermode_faults();
    int rc = usermode_run_calls(&c, data, len, FAULT_ALL, nth);
    *faulted = usermode_faults() != f0;
    return rc;
}

/* "<test>: <why>" first, the line `cxk test sabotage` reads as the step. */
static void sw_fail(const char *test, const char *why, uint32_t nr, uint32_t n, int rc) {
    char line[128];
    uint32_t i = 0;
    while (*test && i < 60) line[i++] = *test++;
    line[i++] = ':'; line[i++] = ' ';
    for (const char *w = why; *w && i < sizeof line - 1; w++) line[i++] = *w;
    line[i] = 0;
    klog("KTEST", SEV_FAIL, line);
    klog_child_u32("syscall ", nr, LOG_COLOR_VALUE, "");
    if (n) klog_child_u32("failing allocation ", n, LOG_COLOR_VALUE, "");
    klog_child_u32("answered ", (uint32_t)rc, LOG_COLOR_VALUE, " (as u32)");
}

/* ---- the census ----
   Every argument 0, then every argument HOSTILE: as a pointer it is in the
   kernel half, as a u32 a huge size, as an i32 a negative handle. A call
   that made a handle anyway (one with no arguments to refuse, or one whose
   0 means "none") has it closed before the counts are compared. */

#define HOSTILE 0xFFFF8000DEAD0FF0ull

/* Not called: these end the caller (EXIT, THREAD_EXIT), wait for a key
   that never comes (INPUT_READ), or act on the machine if their grant check
   broke (the power calls, whose refusals usermode.c's tests show). */
static int census_skipped(uint32_t nr) {
    return nr == SYS_EXIT || nr == SYS_THREAD_EXIT || nr == SYS_INPUT_READ
        || nr == SYS_POWER_REBOOT || nr == SYS_POWER_SHUTDOWN || nr == SYS_POWER_SLEEP;
}
#define CENSUS_SKIPPED 6

/* One run, and what it made closed: a handle more than `before` had, when
   the call answered with one. */
static int census_run(uint32_t nr, uint64_t arg, uint32_t nth, int *faulted,
                      const struct live_counts *before) {
    const uint64_t call[6] = { nr, arg, arg, arg, arg, arg };
    int rc = sw_ring3(call, 0, 0, nth, faulted);
    struct live_counts now;
    sw_live(&now);
    if (rc > 0 && now.handles == before->handles + 1) htab_close(sw_tab, rc);
    return rc;
}

int ktest_syscall_census(void) {
    static const char *name = "census";
    sw_where();
    uint32_t caps0 = thread_current_caps();
    thread_set_caps(sw_me, 0);
    uint32_t called = 0, failed = 0;
    int ok = 1;

    for (uint32_t nr = 0; nr < SYSCALL_TABLE_SIZE && ok; nr++) {
        if (!syscall_table[nr] || census_skipped(nr)) continue;
        called++;
        for (int p = 0; p < 2 && ok; p++) {
            uint64_t arg = p ? HOSTILE : 0;
            struct live_counts b, a;
            int faulted;
            sw_live(&b);
            census_run(nr, arg, 0, &faulted, &b);       /* warm: a slab made once */

            sw_live(&b);
            int rc0 = census_run(nr, arg, 0, &faulted, &b);
            sw_live(&a);
            const char *why = 0;
            if (faulted)                        why = "ring 3 faulted";
            else if (rc0 == E_INTERNAL)         why = "E_INTERNAL, or not run (-1)";
            else if (live_diff(&b, &a, 1))      why = "left something behind";
            if (why) { sw_fail(name, why, nr, 0, rc0); ok = 0; break; }

            /* Again, its first allocation failed. */
            sw_live(&b);
            int rc1 = census_run(nr, arg, 1, &faulted, &b);
            int fired = fault_fired();
            sw_live(&a);
            if (faulted)                        why = "ring 3 faulted";
            else if (live_diff(&b, &a, 1))      why = "left something behind";
            else if (rc0 < 0 && fired)          why = "a refusal reached an allocation";
            else if (rc0 < 0 && rc1 != rc0)     why = "refused differently armed";
            else if (fired && rc1 != E_NO_MEMORY) why = "a failed allocation was not E_NO_MEMORY";
            else if (!fired && (rc1 < 0) != (rc0 < 0)) why = "answered differently armed";
            if (why) { sw_fail(name, why, nr, 1, rc1); ok = 0; }
            failed += fired;
        }
    }
    thread_set_caps(sw_me, caps0);
    /* Some take no arguments and make a handle (a timer, a queue): their
       first allocation failed, or the armed runs tested nothing. */
    if (ok && failed == 0) {
        sw_fail(name, "no first allocation was failed", 0, 0, 0);
        ok = 0;
    }
    if (ok && called != SYSCALL_COUNT - CENSUS_SKIPPED) {
        sw_fail(name, "did not call every syscall in the table", called, 0, 0);
        ok = 0;
    }
    return ok;
}

/* ---- the sweeps ----
   Each makes one thing from ring 3, with what it needs made beforehand
   (sw_ep, sw_nt, sw_q, sw_mem; held across the sweep, so in both
   snapshots), and its tidy undoes a success. As in ktest_hooks.c: two warm
   runs, then the 1st, 2nd, ... allocation failed until a run reaches no
   Nth; every run leaves the counts as they were, a failed one answers
   E_NO_MEMORY, and a sweep that reaches no allocation fails. Not here:
   SYS_HANDLE_DUPLICATE, which allocates only when the table grows (the
   census fails its first allocation when it does), and SYS_THREAD_CREATE,
   which this thread's process (not a user one) is refused. */

#define SWEEP_MAX 64
#define D(off)    ((uint64_t)USERMODE_DATA_VIRT + (off))

static int32_t sw_ep = -1, sw_nt = -1, sw_q = -1, sw_mem = -1;
static uint8_t sw_data[256];

struct sweep {
    const char *name;
    uint32_t    caps;
    void      (*fill)(uint64_t call[6]);           /* the call and its data */
    void      (*tidy)(int rc);                     /* undo a success */
};

static void sw_zero(void) { for (uint32_t i = 0; i < sizeof sw_data; i++) sw_data[i] = 0; }
static void close_rc(int rc) { if (rc > 0) htab_close(sw_tab, rc); }
static void set6(uint64_t c[6], uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    c[0] = nr; c[1] = a0; c[2] = a1; c[3] = a2; c[4] = a3; c[5] = 0;
}

static void f_pipe(uint64_t c[6]) {
    int32_t *ends = (int32_t *)sw_data;
    ends[0] = ends[1] = -1;
    set6(c, SYS_PIPE_CREATE, PIPE_SIZE_DEFAULT, D(0), 0, 0);
}
static void t_pipe(int rc) {
    const int32_t *ends = (const int32_t *)sw_data;
    if (rc == E_OK) { htab_close(sw_tab, ends[0]); htab_close(sw_tab, ends[1]); }
}
static void f_ep(uint64_t c[6])      { set6(c, SYS_EP_CREATE, 0, 0, 0, 0); }
static void f_ep_mint(uint64_t c[6]) { set6(c, SYS_EP_MINT, (uint32_t)sw_ep, 7, HRIGHT_SEND, 0); }
static void f_nt(uint64_t c[6])      { set6(c, SYS_NOTIFY_CREATE, 0, 0, 0, 0); }
static void f_nt_mint(uint64_t c[6]) { set6(c, SYS_NOTIFY_MINT, (uint32_t)sw_nt, 1, HRIGHT_SIGNAL, 0); }
static void f_timer(uint64_t c[6])   { set6(c, SYS_TIMER_CREATE, 0, 0, 0, 0); }
static void f_queue(uint64_t c[6])   { set6(c, SYS_QUEUE_CREATE, 0, 0, 0, 0); }
static void f_qreg(uint64_t c[6]) {
    set6(c, SYS_QUEUE_REGISTER, (uint32_t)sw_q, (uint32_t)sw_nt, SIGNAL_NOTIFY_PENDING, 5);
}
static void t_qreg(int rc) {
    if (rc == E_OK) syscall_dispatch(SYS_QUEUE_UNREGISTER, (uint32_t)sw_q, 5, 0, 0, 0);
}
static void f_job(uint64_t c[6]) {
    struct job_limits *l = (struct job_limits *)sw_data;
    l->memory = 1u << 20; l->threads = 4; l->priority = 0;
    set6(c, SYS_JOB_CREATE, 0, D(0), 0, 0);
}
static void f_mem(uint64_t c[6])     { set6(c, SYS_MEM_CREATE, 3 * 4096, 0, 0, 0); }
static void map_args(uint32_t object) {
    struct vm_map_args *a = (struct vm_map_args *)sw_data;
    a->length = 3 * 4096;
    a->object = object;
    a->prot   = MPROT_READ | MPROT_WRITE;
}
static void f_map(uint64_t c[6])     { map_args(MOBJ_ANON); set6(c, SYS_VM_MAP, D(0), 0, 0, 0); }
static void f_map_mem(uint64_t c[6]) { map_args((uint32_t)sw_mem); set6(c, SYS_VM_MAP, D(0), 0, 0, 0); }
static void t_map(int rc) {
    const struct vm_map_args *a = (const struct vm_map_args *)sw_data;
    if (rc == E_OK) vm_unmap(thread_current_pid(), a->addr, a->granted);
}
static void f_open(uint64_t c[6]) {
    const char *p = "/Shared/Programs/xtest.xuex";
    for (uint32_t i = 0; (sw_data[i] = (uint8_t)p[i]) != 0; i++) { }
    set6(c, SYS_FILE_OPEN, D(0), FOPEN_READ, 0, 0);
}

static const struct sweep sweeps[] = {
    { "sweep r3: pipe_create",       0,              f_pipe,    t_pipe   },
    { "sweep r3: ep_create",         GRANT_ENDPOINT, f_ep,      close_rc },
    { "sweep r3: ep_mint",           0,              f_ep_mint, close_rc },
    { "sweep r3: notify_create",     0,              f_nt,      close_rc },
    { "sweep r3: notify_mint",       0,              f_nt_mint, close_rc },
    { "sweep r3: timer_create",      0,              f_timer,   close_rc },
    { "sweep r3: queue_create",      0,              f_queue,   close_rc },
    { "sweep r3: queue_register",    0,              f_qreg,    t_qreg   },
    { "sweep r3: job_create",        0,              f_job,     close_rc },
    { "sweep r3: mem_create",        0,              f_mem,     close_rc },
    { "sweep r3: vm_map, anonymous", 0,              f_map,     t_map    },
    { "sweep r3: vm_map, an object", 0,              f_map_mem, t_map    },
    { "sweep r3: file_open",         GRANT_DISK,     f_open,    close_rc },
};

static int sweep_run(const struct sweep *s, uint32_t nth, int *faulted) {
    uint64_t call[6];
    sw_zero();
    s->fill(call);
    thread_set_caps(sw_me, s->caps);
    int rc = sw_ring3(call, sw_data, sizeof sw_data, nth, faulted);
    s->tidy(rc);
    return rc;
}

/* "<name>: N allocations failed in turn", to serial only, as ktest_hooks.c
   says how deep each of its sweeps went. */
static void sweep_note(const char *name, uint32_t n) {
    char line[96];
    uint32_t i = 0;
    while (*name && i < 60) line[i++] = *name++;
    line[i++] = ':'; line[i++] = ' ';
    char d[10]; int k = 0;
    do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) line[i++] = d[--k];
    const char *tail = " allocations failed in turn";
    while (*tail) line[i++] = *tail++;
    line[i] = 0;
    klog_offscreen("KTEST", SEV_OK, line);
}

static int sweep_one(const struct sweep *s) {
    int faulted;
    for (int w = 0; w < 2; w++) {
        int rc = sweep_run(s, 0, &faulted);
        if (rc < 0 || faulted) { sw_fail(s->name, "refused with no failure armed", 0, 0, rc); return 0; }
    }
    for (uint32_t n = 1; n <= SWEEP_MAX; n++) {
        struct live_counts before, after;
        sw_live(&before);
        int rc = sweep_run(s, n, &faulted);
        int fired = fault_fired();
        sw_live(&after);

        const char *why = 0;
        if (faulted)                          why = "ring 3 faulted";
        else if (live_diff(&before, &after, 1)) why = "left something behind";
        else if (fired && rc != E_NO_MEMORY)  why = "a failed allocation was not E_NO_MEMORY";
        else if (!fired && rc < 0)            why = "refused with no failure reached";
        if (why) { sw_fail(s->name, why, 0, n, rc); return 0; }
        if (!fired) {
            sweep_note(s->name, n - 1);
            if (n == 1) { sw_fail(s->name, "no allocation was reached", 0, 0, rc); return 0; }
            return 1;
        }
    }
    sw_fail(s->name, "more allocations than the sweep allows", 0, 0, 0);
    return 0;
}

int ktest_syscall_sweeps(void) {
    sw_where();
    /* A kernel thread's process has no address-space account, so SYS_VM_MAP
       refuses it (E_INVALID_ARGS; usermode_file_test says why): one for the
       sweeps, given back after. Its mappings go at VM_MMAP_BASE, clear of
       the ring-3 pages at R3_BASE. */
    int pid = thread_current_pid();
    vm_proc_init(pid, vm_default_quota(), -1);
    uint32_t caps0 = thread_current_caps();
    thread_set_caps(sw_me, caps0 | GRANT_ENDPOINT);
    sw_ep  = syscall_dispatch(SYS_EP_CREATE, 0, 0, 0, 0, 0);
    sw_nt  = syscall_dispatch(SYS_NOTIFY_CREATE, 0, 0, 0, 0, 0);
    sw_q   = syscall_dispatch(SYS_QUEUE_CREATE, 0, 0, 0, 0, 0);
    sw_mem = syscall_dispatch(SYS_MEM_CREATE, 3 * 4096, 0, 0, 0, 0);
    int ok = sw_ep > 0 && sw_nt > 0 && sw_q > 0 && sw_mem > 0;
    if (!ok) klog("KTEST", SEV_FAIL, "sweep r3: the objects it needs were not made");

    for (uint32_t i = 0; ok && i < sizeof sweeps / sizeof sweeps[0]; i++)
        ok &= sweep_one(&sweeps[i]);

    thread_set_caps(sw_me, caps0);
    int32_t held[4] = { sw_ep, sw_nt, sw_q, sw_mem };
    for (int i = 0; i < 4; i++) if (held[i] > 0) htab_close(sw_tab, held[i]);
    sw_ep = sw_nt = sw_q = sw_mem = -1;
    vm_proc_reset(pid);
    return ok;
}
