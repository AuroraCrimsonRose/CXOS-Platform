// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/sync/lockorder.c */
/* Aurora Tejeda / CATX Systems */
/* The lock order's checker (lockorder.h). */
#include "lockorder.h"

static const char *const names[LR_COUNT] = {
    [LR_NONE]         = "(no rank)",
    [LR_STORAGE]      = "storage",
    [LR_IPC]          = "ipc",
    [LR_FUTEX]        = "futex",
    [LR_HTAB]         = "htab",
    [LR_VMREGION]     = "vmregion",
    [LR_PIDS]         = "pids",
    [LR_KMEM]         = "kmem",
    [LR_JOB]          = "job",
    [LR_KTIMER]       = "ktimer",
    [LR_KWAIT]        = "kwait",
    [LR_SYSFILE]      = "sysfile",
    [LR_INPUT]        = "input",
    [LR_PROC]         = "proc",
    [LR_SCHED]        = "sched",
    [LR_RANDOM]       = "random",
    [LR_OCACHE]       = "ocache",
    [LR_OCACHE_SLOTS] = "ocache-slots",
    [LR_HEAP]         = "heap",
    [LR_DMA]          = "dma",
    [LR_KSTACK]       = "kstack",
    [LR_PAGEDB]       = "pagedb",
    [LR_CMOS]         = "cmos",
    [LR_DEVTREE]      = "devtree",
    [LR_PCI]          = "pci",
    [LR_LOG]          = "log",
};

const char *lockorder_name(uint32_t rank) {
    return rank < LR_COUNT && names[rank] ? names[rank] : "(bad rank)";
}

int lockorder_take(struct lockorder_held *h, uint32_t seen[LR_COUNT],
                   const void *lock, uint32_t rank, uintptr_t at, uint32_t *who) {
    if (rank == LR_NONE || rank >= LR_COUNT) return LO_UNRANKED;
    uint32_t top = LR_NONE;
    for (uint32_t i = 0; i < h->n; i++) {
        if (h->e[i].rank >= rank) { *who = i; return LO_ORDER; }
        if (h->e[i].rank > top) top = h->e[i].rank;
    }
    if (h->n == LOCKORDER_DEPTH) return LO_DEPTH;
    h->e[h->n].lock = lock;
    h->e[h->n].rank = rank;
    h->e[h->n].at   = at;
    h->n++;
    __atomic_fetch_or(&seen[top], 1u << rank, __ATOMIC_RELAXED);
    return LO_OK;
}

int lockorder_give(struct lockorder_held *h, const void *lock) {
    for (uint32_t i = h->n; i-- > 0; ) {
        if (h->e[i].lock != lock) continue;
        for (uint32_t j = i + 1; j < h->n; j++) h->e[j - 1] = h->e[j];
        h->n--;
        return LO_OK;
    }
    return LO_NOT_HELD;
}

_Static_assert(LR_COUNT <= 32, "lockorder_seen holds a rank per bit");

#if !defined(CXK_HOST_TEST) && CXK_LOCK_CHECK
#include "panic.h"
#include "lib/string/format.h"
#include "lib/string/logging.h"
#include "cpu/percpu.h"

uint32_t lockorder_seen[LR_COUNT];
/* What this CPU holds: its own (struct cpu), read with interrupts off,
   which every caller has (spin_lock, a switch). */
#define held (cpu_this()->locks)
static volatile int stopped;

/* Armed by a self-test: a failure is counted instead of a panic. */
static volatile int      test_armed;
static volatile uint32_t test_code, test_count;

void lockorder_stop(void) { stopped = 1; }
uint32_t lockorder_depth(void) { return held.n; }

void lockorder_test_arm(void) { test_code = LO_OK; test_count = 0; test_armed = 1; }
uint32_t lockorder_test_disarm(uint32_t *count) {
    test_armed = 0;
    if (count) *count = test_count;
    return test_code;
}

/* "<what> <name>@<hex>" appended at *p. */
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_at(char *p, const char *name, uintptr_t at) {
    p = put(p, name);
    p = put(p, "@0x");
    p += fmt_hex(p, (uint32_t)(at >> 32), 8, 0);
    p += fmt_hex(p, (uint32_t)at, 8, 0);
    return p;
}

/* Panics naming the lock and every lock held, unless a self-test armed the
   checker, which then counts the failure and returns. */
static void fail(uint32_t code, const char *why, uint32_t rank, uintptr_t at) {
    if (test_armed) {
        if (!test_count++) test_code = code;
        return;
    }
    static char msg[96 + LOCKORDER_DEPTH * 40];
    stopped = 1;
    char *p = put(msg, "lock order: ");
    p = put(p, why);
    p = put(p, " ");
    p = put_at(p, lockorder_name(rank), at);
    p = put(p, "; held:");
    for (uint32_t i = 0; i < held.n; i++) {
        p = put(p, " ");
        p = put_at(p, lockorder_name(held.e[i].rank), held.e[i].at);
    }
    *p = 0;
    kpanic(msg);
}

void lockorder_acquire(const void *lock, uint32_t rank, uintptr_t at) {
    if (stopped) return;
    uint32_t who = 0;
    int r = lockorder_take(&held, lockorder_seen, lock, rank, at, &who);
    if (r == LO_OK) return;
    fail((uint32_t)r, r == LO_UNRANKED ? "unranked lock taken:"
                    : r == LO_DEPTH    ? "too many held, taking"
                    :                    "out of order, taking", rank, at);
    /* Armed: an out-of-order lock is still listed, so its release matches. */
    if (r == LO_ORDER && held.n < LOCKORDER_DEPTH) {
        held.e[held.n].lock = lock;
        held.e[held.n].rank = rank;
        held.e[held.n].at   = at;
        held.n++;
    }
}

void lockorder_release(const void *lock) {
    if (stopped) return;
    if (lockorder_give(&held, lock) != LO_OK)
        fail(LO_NOT_HELD, "released, not held:", LR_NONE, (uintptr_t)lock);
}

void lockorder_assert_none(const char *where) {
    if (stopped || held.n == 0) return;
    fail(LO_SWITCH, where, held.e[0].rank, held.e[0].at);
    held.n = 0;   /* armed: the next thread starts with nothing held */
}

void lockorder_assert_switch(const void *sched_lock, const char *where) {
    if (stopped) return;
    uint32_t mine = held.n, other = held.n;
    for (uint32_t i = 0; i < held.n; i++) {
        if (held.e[i].lock == sched_lock) mine = i;
        else if (other == held.n) other = i;
    }
    if (mine == held.n) {
        fail(LO_SWITCH_UNLOCKED, "thread switch without", LR_SCHED,
             (uintptr_t)__builtin_return_address(0));
        return;
    }
    if (other == held.n) return;
    fail(LO_SWITCH, where, held.e[other].rank, held.e[other].at);
    /* Armed: the sched lock alone stays listed, for the next thread to give
       back; the rest are dropped, as lockorder_assert_none drops them. */
    held.e[0] = held.e[mine];
    held.n = 1;
}

/* `s` at `p`, as much as fits before `end` (which stays free for the 0). */
static char *put_in(char *p, char *end, const char *s) {
    while (*s && p < end) *p++ = *s++;
    return p;
}

/* One line per outer rank that nested anything: "ipc > htab kwait". Room
   for every rank's name (the (none) line names nearly all of them); a name
   past it is cut, never written beyond the line (2.2: "pci" took a 160-byte
   line over its end and the stack with it). */
#define REPORT_NAME_MAX 16u
void lockorder_report(void) {
    char line[LR_COUNT * (REPORT_NAME_MAX + 1u) + 16u];
    char *end = line + sizeof line - 1;
    for (uint32_t i = 0; i < LR_COUNT; i++) {
        if (!lockorder_seen[i]) continue;
        char *p = put_in(line, end, i == LR_NONE ? "(none)" : lockorder_name(i));
        p = put_in(p, end, " >");
        for (uint32_t j = 1; j < LR_COUNT; j++)
            if (lockorder_seen[i] & (1u << j)) {
                p = put_in(p, end, " ");
                p = put_in(p, end, lockorder_name(j));
            }
        *p = 0;
        klog_offscreen("LOCKS", SEV_INFO, line);
    }
}
#endif
