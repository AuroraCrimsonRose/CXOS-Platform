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
#include "ipc.h"
#include "logging.h"
#include "object/kobj.h"
#include "object/kmem.h"
#include "object/htab.h"
#include "object/kwait.h"
#include "object/kqueue.h"

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

/* ==== IPC and queues (1.6c2) ====
   The callers are ring-3 processes (usermode_calls_process: two
   SYS_THREAD_IDs, then the call), the server is the test thread through
   syscall_dispatch, as in usermode.c's IPC self-tests. Their arguments and
   buffers sit in one mapping of this thread's process, every page touched
   first: the ring-3 helpers share the kernel's page tables, so they see it
   too, at the same address. */

#define IR_BASE     (VM_MMAP_BASE + 0x400000u)
#define IR_LEN      0x21000u
#define IR_CALL(i)  (IR_BASE + (i) * 0x40u)           /* caller i's ipc_call_args, i < 4 */
#define IR_RECV     (IR_BASE + 0x100u)                /* the test thread's ipc_recv_args */
#define IR_INFO     (IR_BASE + 0x140u)                /* its ipc_msg_info */
#define IR_RARGS    (IR_BASE + 0x180u)                /* its ipc_reply_args */
#define IR_HRECV    (IR_BASE + 0x1C0u)                /* a kernel helper's ipc_recv_args */
#define IR_HINFO    (IR_BASE + 0x200u)                /* its ipc_msg_info */
#define IR_EVENTS   (IR_BASE + 0x240u)                /* a queue waiter's events */
#define IR_REQ(i)   (IR_BASE + 0x400u + (i) * 0x40u)  /* caller i's request */
#define IR_REPLY(i) (IR_BASE + 0x600u + (i) * 0x40u)  /* its reply buffer */
#define IR_BUF      (IR_BASE + 0x800u)                /* the test thread's receive buffer, 64 bytes */
#define IR_DATA     (IR_BASE + 0x900u)                /* its reply bytes */
#define IR_BIGA     (IR_BASE + 0x1000u)               /* IPC_MSG_MAX: a caller's request and reply */
#define IR_BIGB     (IR_BASE + 0x11000u)              /* IPC_MSG_MAX: the server's */
_Static_assert(IR_BIGB + IPC_MSG_MAX == IR_BASE + IR_LEN, "the layout fills the mapping");

struct ir {
    int pid, mapped;
    uint32_t threads0, procs0;
    struct vm_map_args map;
    struct proc *p[2];
    int tid[2];
};

/* The mapping at IR_BASE, every page touched. 1, or 0 if not made. */
static int ir_map(int pid, struct vm_map_args *a) {
    uint8_t *b = (uint8_t *)a;
    for (uint32_t i = 0; i < sizeof *a; i++) b[i] = 0;
    a->object = MOBJ_ANON;
    a->length = IR_LEN;
    a->prot   = MPROT_READ | MPROT_WRITE;
    a->flags  = MMAP_HINT;
    a->addr   = IR_BASE;
    if (vm_map_object(pid, a, 0, HRIGHT_MAP | HRIGHT_MAP_WRITE) != E_OK) return 0;
    int ok = a->addr == IR_BASE;
    for (uint64_t p = IR_BASE; ok && p < IR_BASE + IR_LEN; p += 4096)
        ok = vm_fault(pid, p, 1, 0) != 0;
    if (!ok) vm_unmap(pid, a->addr, a->granted);
    return ok;
}

static int ir_begin(struct ir *s) {
    s->pid = thread_current_pid();
    s->threads0 = sched_thread_count();
    s->procs0 = proc_live();
    s->p[0] = s->p[1] = 0;
    s->tid[0] = s->tid[1] = -1;
    vm_proc_init(s->pid, vm_default_quota(), -1);
    s->mapped = ir_map(s->pid, &s->map);
    return s->mapped;
}

/* Every helper still running is killed and waited for; `why` stays the
   first failure. */
static const char *ir_end(struct ir *s, const char *why, const char *stuck) {
    for (int i = 0; i < 2; i++) {
        if (!s->p[i]) continue;
        if (s->p[i]->ended == PROC_RUNNING) proc_kill(s->p[i], s->pid);
        proc_put(s->p[i]);
        s->p[i] = 0;
    }
    if (!rc_settle(s->threads0, s->procs0) && !why) why = stuck;
    if (s->mapped) vm_unmap(s->pid, s->map.addr, s->map.granted);
    vm_proc_reset(s->pid);
    return why;
}

/* Caller i: a ring-3 process calling `ep` with `req_len` bytes at `req`
   and a reply of up to `rcap` into `reply`, at `level` (0: as made), then
   started. It ends with what the call returned. 0 if not made. */
static struct proc *ir_caller(struct endpoint *ep, uint32_t i, uint64_t req, uint64_t req_len,
                              uint64_t reply, uint64_t rcap, uint32_t level, int *tid) {
    struct um_call3 c;
    rc_set(c.c[0], SYS_THREAD_ID, 0, 0, 0);
    rc_set(c.c[1], SYS_THREAD_ID, 0, 0, 0);
    rc_set(c.c[2], SYS_IPC_CALL, IR_CALL(i), 0, 0);
    struct proc *p = usermode_calls_process(&c, tid);
    if (!p) return 0;
    int32_t v = ep_install_send(*tid, ep, 0, HRIGHT_SEND, 0);
    if (v <= 0) { thread_discard(*tid); proc_put(p); return 0; }
    struct ipc_call_args *a = (struct ipc_call_args *)user_ptr(IR_CALL(i));
    uint8_t *b = (uint8_t *)a;
    for (uint32_t k = 0; k < sizeof *a; k++) b[k] = 0;
    a->req_len = req_len; a->reply_cap = rcap; a->req = req; a->reply = reply;
    a->ep_handle = v;
    if (level) ipc_set_base(*tid, level);
    thread_start(*tid);
    return p;
}

/* `n` calls queued on `ep`, within a second and a half. */
static int ir_queued(struct endpoint *ep, uint32_t n) {
    uint64_t end = timer_now_ns() + 1500 * MS;
    while (ep_queued(ep) != n && timer_now_ns() < end) yield();
    return ep_queued(ep) == n;
}

/* `p` ended as `how` (and with `code`, if it exited) within `ms`. */
static int ir_ends(struct proc *p, uint32_t how, int32_t code, uint32_t ms) {
    uint64_t end = timer_now_ns() + ms * MS;
    while (p->ended == PROC_RUNNING && timer_now_ns() < end) yield();
    return p->ended == how && (how != PROC_EXITED || p->end_code == code);
}

/* `p`'s threads all through thread_exit within `ms`: when, or 0. */
static uint64_t ir_gone(struct proc *p, uint32_t ms) {
    uint64_t end = timer_now_ns() + ms * MS;
    while (p->running && timer_now_ns() < end) yield();
    return p->running ? 0 : timer_now_ns();
}

static int ir_recv(int32_t eh, uint64_t buf, uint64_t cap) {
    struct ipc_recv_args *r = (struct ipc_recv_args *)user_ptr(IR_RECV);
    r->cap = cap; r->buf = buf; r->info = IR_INFO; r->handles = 0;
    r->ep_handle = eh; r->handle_cap = 0;
    return syscall_dispatch(SYS_IPC_RECV, IR_RECV, 0, 0, 0, 0);
}

static int ir_reply(int32_t tok, uint64_t data, uint64_t len) {
    struct ipc_reply_args *r = (struct ipc_reply_args *)user_ptr(IR_RARGS);
    r->data = data; r->len = len; r->handles = 0; r->token = tok; r->handle_count = 0;
    return syscall_dispatch(SYS_IPC_REPLY, IR_RARGS, 0, 0, 0, 0);
}

/* ---- a reply racing its token's close ---- */

static int32_t rp_tok;
static int     rp_closed;
static struct kobj *rp_call[2];   /* each round's call, held by the test */

/* Another thread of the server closes the token as the reply reads its
   bytes (the same table: the test thread's). */
static void rp_close(void *ctx) {
    (void)ctx;
    rp_closed = htab_close(thread_htab(thread_current_id()), rp_tok);
}

/* Caller i's call taken here, its token in rp_tok; then the reply, from
   `data`, with the token closed at the point. */
static const char *rp_round(struct ir *s, int32_t eh, struct endpoint *ep, int i,
                            uint64_t data, int *rc) {
    const struct ipc_msg_info *in = (const struct ipc_msg_info *)user_ptr(IR_INFO);
    s->p[i] = ir_caller(ep, (uint32_t)i, IR_REQ(i), 1, IR_REPLY(i), 8, 0, &s->tid[i]);
    if (!s->p[i] || !ir_queued(ep, 1)) return "reply close: setup caller";
    if (ir_recv(eh, IR_BUF, 64) != 1 || in->reply <= 0) return "reply close: setup recv";
    rp_tok = in->reply;
    rp_call[i] = htab_ref(thread_htab(thread_current_id()), rp_tok, KOBJ_REPLY, 0, 0);
    rp_closed = 1;
    if (race_arm(RP_IPC_REPLY_COPY, thread_current_id(), 1, RACE_CALL, rp_close, 0) != 0)
        return "reply close: setup arm";
    *rc = ir_reply(rp_tok, data, 3);
    uint32_t arrived = race_arrivals(RP_IPC_REPLY_COPY);
    race_release(RP_IPC_REPLY_COPY);
    if (!arrived)          return "reply close: point not reached";
    if (rp_closed != E_OK) return "reply close: the close failed";
    return 0;
}

static const char *reply_close_steps(struct ir *s, int32_t eh, struct endpoint *ep) {
    uint8_t *data = (uint8_t *)user_ptr(IR_DATA);
    data[0] = 'r'; data[1] = 'e'; data[2] = 'p';
    int rc = 0;

    /* read as its token closes: the reply is the caller's all the same */
    const char *why = rp_round(s, eh, ep, 0, IR_DATA, &rc);
    if (why) return why;
    if (rc != E_OK) return "reply close: reply not E_OK";
    if (!ir_ends(s->p[0], PROC_EXITED, 3, 1500)) return "reply close: caller lost the reply";
    const uint8_t *rb = (const uint8_t *)user_ptr(IR_REPLY(0));
    if (rb[0] != 'r' || rb[2] != 'p') return "reply close: reply bytes wrong";

    /* unreadable as its token closes: nobody holds the call any more, so
       the caller is told, not left waiting */
    why = rp_round(s, eh, ep, 1, USER_VLIMIT, &rc);
    if (why) return why;
    if (rc != E_BAD_ADDRESS) return "reply close: not E_BAD_ADDRESS";
    if (!ir_ends(s->p[1], PROC_EXITED, E_PEER_CLOSED, 1500)) return "reply close: caller left waiting";
    return 0;
}

int ktest_ipc_reply_close(void) {
    struct ir s;
    struct htab *tab = thread_htab(thread_current_id());
    uint32_t calls0 = kobj_live(KOBJ_REPLY);
    rp_closed = E_OK;
    const char *why = ir_begin(&s) ? 0 : "reply close: setup map";
    int32_t eh = why ? -1 : ep_create();
    struct endpoint *ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;
    if (!why) why = ep ? reply_close_steps(&s, eh, ep) : "reply close: setup endpoint";
    race_release(RP_IPC_REPLY_COPY);
    if (rp_closed != E_OK) htab_close(tab, rp_tok);   /* a token the point never closed */
    /* A call whose caller is still waiting was never finished, and may still
       be on the serving list: freeing it with the caller's kill would leave
       that list pointing at freed memory. It is leaked, and the test fails. */
    for (int i = 0; i < 2; i++) {
        if (rp_call[i] && !(s.p[i] && s.p[i]->ended == PROC_RUNNING)) kobj_unref(rp_call[i]);
        rp_call[i] = 0;
    }
    if (ep) ep_put(ep);
    if (eh > 0) htab_close(tab, eh);
    why = ir_end(&s, why, "reply close: the callers never went");
    if (!why && kobj_live(KOBJ_REPLY) != calls0) why = "reply close: a call left behind";
    return rc_report(why);
}

/* ---- IPC_MSG_MAX each way ---- */

static uint8_t mm_req(uint32_t i)   { return (uint8_t)(i * 7 + 1); }
static uint8_t mm_reply(uint32_t i) { return (uint8_t)(i * 13 + 5); }

static const char *msg_max_steps(struct ir *s, int32_t eh, struct endpoint *ep, int32_t *tok) {
    const struct ipc_msg_info *in = (const struct ipc_msg_info *)user_ptr(IR_INFO);
    uint8_t *a = (uint8_t *)user_ptr(IR_BIGA), *b = (uint8_t *)user_ptr(IR_BIGB);
    for (uint32_t i = 0; i < IPC_MSG_MAX; i++) a[i] = mm_req(i);
    s->p[0] = ir_caller(ep, 0, IR_BIGA, IPC_MSG_MAX, IR_BIGA, IPC_MSG_MAX, 0, &s->tid[0]);
    if (!s->p[0]) return "msg max: setup caller";
    if (!ir_queued(ep, 1)) return "msg max: the largest call refused";

    /* a byte short of room: told its length, given nothing */
    int rc = ir_recv(eh, IR_BIGB, IPC_MSG_MAX - 1);
    if (rc > 0) *tok = in->reply;
    if (rc != E_BUFFER_TOO_SMALL || in->len != IPC_MSG_MAX) return "msg max: a byte short taken";

    rc = ir_recv(eh, IR_BIGB, IPC_MSG_MAX);
    if (rc > 0) *tok = in->reply;
    if (rc != IPC_MSG_MAX) return "msg max: largest receive refused";
    if (in->reply_cap != IPC_MSG_MAX) return "msg max: reply_cap not the max";
    for (uint32_t i = 0; i < IPC_MSG_MAX; i++)
        if (b[i] != mm_req(i)) return "msg max: request bytes wrong";

    for (uint32_t i = 0; i < IPC_MSG_MAX; i++) b[i] = mm_reply(i);
    if (ir_reply(*tok, IR_BIGB, IPC_MSG_MAX + 1) != E_BUFFER_TOO_SMALL) return "msg max: a reply too long taken";
    if (ir_reply(*tok, IR_BIGB, IPC_MSG_MAX) != E_OK) return "msg max: largest reply refused";
    *tok = -1;
    if (!ir_ends(s->p[0], PROC_EXITED, IPC_MSG_MAX, 3000)) return "msg max: caller lost the reply";
    for (uint32_t i = 0; i < IPC_MSG_MAX; i++)
        if (a[i] != mm_reply(i)) return "msg max: reply bytes wrong";
    return 0;
}

int ktest_ipc_msg_max(void) {
    struct ir s;
    struct htab *tab = thread_htab(thread_current_id());
    uint32_t calls0 = kobj_live(KOBJ_REPLY);
    int32_t tok = -1;
    const char *why = ir_begin(&s) ? 0 : "msg max: setup map";
    int32_t eh = why ? -1 : ep_create();
    struct endpoint *ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;
    if (!why) why = ep ? msg_max_steps(&s, eh, ep, &tok) : "msg max: setup endpoint";
    if (tok > 0) htab_close(tab, tok);
    if (ep) ep_put(ep);
    if (eh > 0) htab_close(tab, eh);
    why = ir_end(&s, why, "msg max: the caller never went");
    if (!why && kobj_live(KOBJ_REPLY) != calls0) why = "msg max: a call left behind";
    return rc_report(why);
}

/* ---- an endpoint closed under a call it gave out ----
   A taken call names the endpoint it came from, for the lift its queue
   gives the server (ipc.c, "Priority through calls"). The endpoint can go
   while the call is still answered: its last receive handle closed, its
   caller killed. The call must not name it after that. */

static const char *detach_steps(struct ir *s, int32_t *eh, struct endpoint **ep,
                                int32_t *tok, struct kobj **co, int eps0) {
    struct htab *tab = thread_htab(thread_current_id());
    const struct ipc_msg_info *in = (const struct ipc_msg_info *)user_ptr(IR_INFO);

    /* A's call, taken here; A killed while it is answered */
    s->p[0] = ir_caller(*ep, 0, IR_REQ(0), 1, IR_REPLY(0), 8, 0, &s->tid[0]);
    if (!s->p[0] || !ir_queued(*ep, 1)) return "detach: setup caller";
    if (ir_recv(*eh, IR_BUF, 64) != 1 || in->reply <= 0) return "detach: setup recv";
    *tok = in->reply;
    *co = htab_ref(tab, *tok, KOBJ_REPLY, 0, 0);
    if (!*co || ((struct ipc_call *)*co)->from != *ep) return "detach: setup the call";
    proc_kill(s->p[0], s->pid);
    if (!ir_gone(s->p[0], 1500)) return "detach: setup kill";

    /* the receive handle closed: nothing holds the endpoint now */
    ep_put(*ep); *ep = 0;
    htab_close(tab, *eh); *eh = -1;
    if (((struct ipc_call *)*co)->from != 0) return "detach: the call names its endpoint";
    if (ep_in_use_count() != eps0) return "detach: setup the endpoint stayed";

    /* another endpoint, maybe in the freed slot, with a caller above this
       thread queued: it lifts nothing here */
    *eh = ep_create();
    *ep = *eh > 0 ? ep_from_handle(*eh, HRIGHT_RECV, 0) : 0;
    if (!*ep) return "detach: setup endpoint";
    s->p[1] = ir_caller(*ep, 1, IR_REQ(1), 1, IR_REPLY(1), 8, PRIO_MAX, &s->tid[1]);
    if (!s->p[1] || !ir_queued(*ep, 1)) return "detach: setup caller";
    if (thread_prio(thread_current_id()) != thread_base(thread_current_id()))
        return "detach: lifted by another queue";
    return 0;
}

int ktest_ipc_from_detach(void) {
    struct ir s;
    struct htab *tab = thread_htab(thread_current_id());
    uint32_t calls0 = kobj_live(KOBJ_REPLY), serving0 = ipc_serving_count();
    int eps0 = ep_in_use_count();
    int32_t tok = -1;
    struct kobj *co = 0;
    const char *why = ir_begin(&s) ? 0 : "detach: setup map";
    int32_t eh = why ? -1 : ep_create();
    struct endpoint *ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;
    if (!why) why = ep ? detach_steps(&s, &eh, &ep, &tok, &co, eps0) : "detach: setup endpoint";
    if (co) kobj_unref(co);
    if (tok > 0) htab_close(tab, tok);   /* the call fails; its caller is gone */
    if (ep) ep_put(ep);
    if (eh > 0) htab_close(tab, eh);      /* B fails */
    why = ir_end(&s, why, "detach: the callers never went");
    if (!why && (kobj_live(KOBJ_REPLY) != calls0 || ipc_serving_count() != serving0
                 || ep_in_use_count() != eps0))
        why = "detach: left something behind";
    return rc_report(why);
}

/* ---- a server thread ends with a call taken ----
   A kernel thread takes the call and hands its token to the test thread
   (as a server passes one to a worker), then ends. The call must stop
   naming it as its server: the id is free for the next thread. */

static volatile int     tx_go, tx_done;
static volatile int32_t tx_eh, tx_tok;
static struct htab     *tx_to;

static void tx_server(void) {
    while (!tx_go) yield();
    struct ipc_recv_args *r = (struct ipc_recv_args *)user_ptr(IR_HRECV);
    const struct ipc_msg_info *in = (const struct ipc_msg_info *)user_ptr(IR_HINFO);
    struct htab *mine = thread_htab(thread_current_id());
    r->cap = 0; r->buf = 0; r->info = IR_HINFO; r->handles = 0;
    r->ep_handle = tx_eh; r->handle_cap = 0;
    if (syscall_dispatch(SYS_IPC_RECV, IR_HRECV, 0, 0, 0, 0) == 0 && in->reply > 0) {
        struct kobj *o = htab_ref(mine, in->reply, KOBJ_REPLY, HRIGHT_REPLY, 0);
        if (o) {
            tx_tok = htab_install(tx_to, o, HRIGHT_REPLY, 0);
            kobj_unref(o);
        }
        htab_close(mine, in->reply);
    }
    tx_done = 1;
}

static const char *texit_steps(struct ir *s, struct endpoint *ep, uint32_t serving0) {
    tx_go = tx_done = 0;
    tx_tok = -1;
    tx_to = thread_htab(thread_current_id());
    int t = thread_create("ipc-server", tx_server);
    if (t < 0) return "thread exit: setup server";
    tx_eh = ep_install_handle(t, ep, HRIGHT_RECV, 0);
    s->p[0] = tx_eh > 0 ? ir_caller(ep, 0, IR_REQ(0), 0, IR_REPLY(0), 8, 0, &s->tid[0]) : 0;
    int queued = s->p[0] && ir_queued(ep, 1);
    tx_go = 1;   /* with no handle or no call, it fails at once and ends */
    if (!queued) return "thread exit: setup caller";

    uint64_t end = timer_now_ns() + 1500 * MS;
    while ((!tx_done || thread_base(t)) && timer_now_ns() < end) yield();
    if (tx_tok <= 0)    return "thread exit: setup the token";
    if (thread_base(t)) return "thread exit: setup the server stayed";
    if (ipc_serving_count() != serving0) return "thread exit: a gone thread serves";

    /* the token still answers it */
    int rc = ir_reply(tx_tok, IR_DATA, 0);
    tx_tok = -1;
    if (rc != E_OK) return "thread exit: reply refused";
    if (!ir_ends(s->p[0], PROC_EXITED, 0, 1500)) return "thread exit: caller lost the reply";
    return 0;
}

int ktest_ipc_thread_exit(void) {
    struct ir s;
    uint32_t calls0 = kobj_live(KOBJ_REPLY), serving0 = ipc_serving_count();
    const char *why = ir_begin(&s) ? 0 : "thread exit: setup map";
    int32_t eh = why ? -1 : ep_create();
    struct endpoint *ep = eh > 0 ? ep_from_handle(eh, HRIGHT_RECV, 0) : 0;
    if (!why) why = ep ? texit_steps(&s, ep, serving0) : "thread exit: setup endpoint";
    if (tx_tok > 0) htab_close(thread_htab(thread_current_id()), tx_tok);
    if (ep) ep_put(ep);
    if (eh > 0) htab_close(thread_htab(thread_current_id()), eh);
    why = ir_end(&s, why, "thread exit: the threads never went");
    if (!why && (kobj_live(KOBJ_REPLY) != calls0 || ipc_serving_count() != serving0))
        why = "thread exit: left something behind";
    return rc_report(why);
}

/* ---- queues: one key, two waiters; and a kill in the wait ----
   A queue with one key, an endpoint's READABLE, made ready by a stop
   notice posted on it (and never taken: it stays readable). */

#define QK_KEY 0x5151u

struct qk {
    int32_t qh, eh;
    struct kqueue *q;
    struct endpoint *ep;
};

static const char *qk_begin(struct qk *k) {
    struct htab *tab = thread_htab(thread_current_id());
    k->qh = syscall_dispatch(SYS_QUEUE_CREATE, 0, 0, 0, 0, 0);
    k->q  = k->qh > 0 ? (struct kqueue *)htab_ref(tab, k->qh, KOBJ_QUEUE, 0, 0) : 0;
    k->eh = ep_create();
    k->ep = k->eh > 0 ? ep_from_handle(k->eh, HRIGHT_RECV, 0) : 0;
    if (!k->q || !k->ep) return "queue: setup";
    if (kqueue_register(k->q, &k->ep->hdr, SIGNAL_EP_READABLE, QK_KEY) != E_OK) return "queue: setup register";
    if (ep_post_stop(k->ep, 0, 0) != E_OK) return "queue: setup ready";
    return 0;
}

static void qk_end(struct qk *k) {
    struct htab *tab = thread_htab(thread_current_id());
    if (k->q) kobj_unref(&k->q->hdr);
    if (k->qh > 0) htab_close(tab, k->qh);   /* the registration goes with the queue */
    if (k->ep) ep_put(k->ep);
    if (k->eh > 0) htab_close(tab, k->eh);
}

/* SYS_QUEUE_WAIT on `qh` for one event into `events`, third. */
static void qk_calls(struct um_call3 *c, int32_t qh, uint64_t events, uint64_t deadline) {
    rc_set(c->c[0], SYS_THREAD_ID, 0, 0, 0);
    rc_set(c->c[1], SYS_THREAD_ID, 0, 0, 0);
    rc_set(c->c[2], SYS_QUEUE_WAIT, (uint32_t)qh, events, 1);
#if SYSCALL_PAIRS
    c->c[2][4] = (uint32_t)deadline;
    c->c[2][5] = deadline >> 32;
#else
    c->c[2][4] = deadline;
#endif
}

static struct kqueue      *qt_q;
static struct queue_event  qt_ev;
static int                 qt_got;

/* Another waiter, between this one's wake and its look. */
static void qt_take(void *ctx) { (void)ctx; qt_got = kqueue_wait(qt_q, &qt_ev, 1, 0); }

static const char *taken_steps(struct qk *k) {
    struct queue_event ev = { 0, 0, 0 };
    struct um_call3 c;

    /* unarmed: the event is there and the wait takes it */
    qk_calls(&c, k->qh, USERMODE_DATA_VIRT, timer_now_ns() + 50 * MS);
    int rc = usermode_run_calls(&c, &ev, sizeof ev, 0, 0);
    if (rc != 1 || ev.key != QK_KEY) return "queue taken: unarmed not 1 event";
    if (kqueue_arm(k->q, QK_KEY) != E_OK) return "queue taken: setup arm key";

    qt_q = k->q;
    qt_got = -1;
    if (race_arm(RP_QUEUE_WOKEN, thread_current_id(), 1, RACE_CALL, qt_take, 0) != 0)
        return "queue taken: setup arm";
    qk_calls(&c, k->qh, USERMODE_DATA_VIRT, timer_now_ns() + 50 * MS);
    rc = usermode_run_calls(&c, &ev, sizeof ev, 0, 0);
    uint32_t arrived = race_arrivals(RP_QUEUE_WOKEN);
    race_release(RP_QUEUE_WOKEN);
    if (!arrived) return "queue taken: point not reached";
    if (qt_got != 1 || qt_ev.key != QK_KEY) return "queue taken: setup the take";
    if (rc != E_TIMED_OUT) return "queue taken: not E_TIMED_OUT";
    return 0;
}

int ktest_queue_taken(void) {
    struct qk k = { -1, -1, 0, 0 };
    uint32_t regs0 = kqueue_reg_count();
    const char *why = qk_begin(&k);
    if (!why) why = taken_steps(&k);
    race_release(RP_QUEUE_WOKEN);
    qk_end(&k);
    if (!why && kqueue_reg_count() != regs0) why = "queue taken: left a registration";
    return rc_report(why);
}

/* A ring-3 process waiting on the queue for one event into IR_EVENTS,
   until `deadline`, made and held (started by the caller). */
static struct proc *qk_waiter(struct qk *k, uint64_t deadline, int *tid) {
    struct um_call3 c;
    qk_calls(&c, 0, IR_EVENTS, deadline);
    struct proc *p = usermode_calls_process(&c, tid);
    if (!p) return 0;
    int32_t v = htab_install(thread_htab(*tid), &k->q->hdr, HRIGHT_WAIT, 0);
    if (v <= 0) { thread_discard(*tid); proc_put(p); return 0; }
    usermode_calls_set(2, 1, (uint32_t)v);
    return p;
}

/* The kill is noted at once; the thread must leave the wait long before
   its deadline (a process ending logs, which costs virtual time). */
#define QK_DEADLINE (10000 * MS)
#define QK_PROMPT   (4000 * MS)

static const char *qkill_steps(struct ir *s, struct qk *k) {
    /* blocked in the wait (the key taken first, and left idle), then
       killed */
    struct queue_event ev;
    if (kqueue_wait(k->q, &ev, 1, 0) != 1) return "queue kill: setup the take";
    s->p[0] = qk_waiter(k, timer_now_ns() + QK_DEADLINE, &s->tid[0]);
    if (!s->p[0]) return "queue kill: setup waiter";
    thread_start(s->tid[0]);
    uint64_t end = timer_now_ns() + 1500 * MS;
    while (kobj_waiters(&k->q->hdr) != 1 && timer_now_ns() < end) yield();
    if (kobj_waiters(&k->q->hdr) != 1) return "queue kill: setup the wait";
    proc_kill(s->p[0], s->pid);
    uint64_t t0 = timer_now_ns(), t1 = ir_gone(s->p[0], 15000);
    if (!t1 || t1 - t0 > QK_PROMPT) return "queue kill: blocked waiter stayed";
    if (kobj_waiters(&k->q->hdr) != 0) return "queue kill: left a waiter";

    /* woken, parked before it looks; the event taken and the kill made
       meanwhile: it finds nothing, and must not block again */
    if (kqueue_arm(k->q, QK_KEY) != E_OK) return "queue kill: setup arm key";
    s->p[1] = qk_waiter(k, timer_now_ns() + QK_DEADLINE, &s->tid[1]);
    if (!s->p[1]) return "queue kill: setup waiter";
    if (race_arm(RP_QUEUE_WOKEN, s->tid[1], 1, RACE_PARK, 0, 0) != 0) {
        thread_discard(s->tid[1]);
        return "queue kill: setup arm";
    }
    thread_start(s->tid[1]);
    if (race_wait_parked(RP_QUEUE_WOKEN, 1500) != s->tid[1]) return "queue kill: point not reached";
    if (kqueue_wait(k->q, &ev, 1, 0) != 1) return "queue kill: setup the take";
    proc_kill(s->p[1], s->pid);
    t0 = timer_now_ns();
    race_release(RP_QUEUE_WOKEN);
    t1 = ir_gone(s->p[1], 15000);
    if (!t1 || t1 - t0 > QK_PROMPT) return "queue kill: woken waiter stayed";
    if (kobj_waiters(&k->q->hdr) != 0) return "queue kill: left a waiter";
    return 0;
}

int ktest_queue_kill(void) {
    struct ir s;
    struct qk k = { -1, -1, 0, 0 };
    uint32_t regs0 = kqueue_reg_count();
    const char *why = ir_begin(&s) ? 0 : "queue kill: setup map";
    if (!why) why = qk_begin(&k);
    if (!why) why = qkill_steps(&s, &k);
    race_release(RP_QUEUE_WOKEN);
    why = ir_end(&s, why, "queue kill: the waiters never went");
    qk_end(&k);
    if (!why && kqueue_reg_count() != regs0) why = "queue kill: left a registration";
    return rc_report(why);
}
