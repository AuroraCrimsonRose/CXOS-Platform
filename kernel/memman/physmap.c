// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/physmap.c */
/* Aurora Tejeda / CATX Systems */
/* Reaching physical memory through short, nested, per-CPU mappings (see
   physmap.h). The architecture places each frame (paging_phys_slot); this
   file keeps the CPU's list of what is open and checks every use of it. */

#include "physmap.h"
#include "arch/cpu_ops.h"
#include "cpu/percpu.h"
#include "panic.h"
#include "lib/string/format.h"

/* What this CPU has open: its own (struct cpu), read with interrupts off. */
#define mine (cpu_this()->phys)
static volatile int stopped;   /* a failure is being reported: check no more */

#if CXK_TEST_HOOKS
static volatile int      test_armed;
static volatile uint32_t test_code, test_count;

void physmap_test_arm(void) { test_code = PM_OK; test_count = 0; test_armed = 1; }
uint32_t physmap_test_disarm(uint32_t *count) {
    test_armed = 0;
    if (count) *count = test_count;
    return test_code;
}
#endif

static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_hex(char *p, uint64_t v) {
    p = put(p, "0x");
    p += fmt_hex(p, (uint32_t)(v >> 32), 8, 0);
    p += fmt_hex(p, (uint32_t)v, 8, 0);
    return p;
}

/* Panics naming the frame and every frame open, unless a self-test armed
   the checker, which then counts the failure and returns. */
static void fail(uint32_t code, const char *why, uint64_t frame) {
#if CXK_TEST_HOOKS
    if (test_armed) {
        if (!test_count++) test_code = code;
        return;
    }
#else
    (void)code;
#endif
    static char msg[80 + PHYS_MAP_DEPTH * 20];
    stopped = 1;
    char *p = put(msg, "physmap: ");
    p = put(p, why);
    p = put(p, " ");
    p = put_hex(p, frame);
    p = put(p, "; open:");
    for (uint32_t i = 0; i < mine.n && i < PHYS_MAP_DEPTH; i++) {
        p = put(p, " ");
        p = put_hex(p, mine.e[i].frame);
    }
    *p = 0;
    kpanic(msg);
}

void *phys_map(paddr_t phys) {
    uint32_t f = arch_irq_save();
    paddr_t frame = phys & ~(paddr_t)(PAGE_SIZE - 1);
    if (stopped) {   /* the panic path: no list to keep */
        void *v = paging_phys_slot(cpu_this()->index, 0, frame);
        return v ? (uint8_t *)v + (phys - frame) : 0;
    }
    if (mine.n >= PHYS_MAP_DEPTH) {
        fail(PM_DEPTH, "more than PHYS_MAP_DEPTH open, mapping", frame);
        arch_irq_restore(f);
        return 0;
    }
    void *v = frame < RAM_REACH ? paging_phys_slot(cpu_this()->index, mine.n, frame) : 0;
    if (!v) {
        fail(PM_REACH, "out of the kernel's reach:", frame);
        arch_irq_restore(f);
        return 0;
    }
    mine.e[mine.n].frame = frame;
    mine.e[mine.n].at    = (uintptr_t)v;
    mine.e[mine.n].flags = f;
    mine.n++;
    return (uint8_t *)v + (phys - frame);
}

void phys_unmap(const volatile void *at) {
    if (stopped) return;
    uintptr_t page = (uintptr_t)at & ~(uintptr_t)(PAGE_SIZE - 1);
    if (mine.n == 0) { fail(PM_NONE_OPEN, "unmapped with none open:", page); return; }
    uint32_t top = mine.n - 1;
    if (mine.e[top].at != page) {
        fail(PM_NOT_LAST, "unmapped, not the most recent:", page);
        return;
    }
    uint32_t f = mine.e[top].flags;
    paging_phys_slot_clear(cpu_this()->index, top);
    mine.n = top;
    arch_irq_restore(f);
}

void phys_zero(paddr_t frame) {
    uint64_t *v = phys_map(frame & ~(paddr_t)(PAGE_SIZE - 1));
    for (uint32_t i = 0; i < PAGE_SIZE / sizeof *v; i++) v[i] = 0;
    phys_unmap(v);
}

void *phys_map_lasting(paddr_t phys, uint64_t len) {
    void *v = (phys < RAM_REACH && len <= RAM_REACH - phys) ? paging_map_ram(phys, len) : 0;
    if (!v) kpanic("physmap: a lasting mapping out of the kernel's reach");
    return v;
}

uint32_t phys_map_depth(void) {
    uint32_t f = arch_irq_save();
    uint32_t n = mine.n;
    arch_irq_restore(f);
    return n;
}

void phys_assert_none(const char *where) {
    if (stopped || mine.n == 0) return;
    fail(PM_SWITCH, where, mine.e[mine.n - 1].frame);
}
