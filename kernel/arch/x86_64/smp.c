// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/smp.c */
/* Aurora Tejeda / CATX Systems */
/*
 * arch/smp.h on x86-64: the APs started by the MADT's IDs and parked
 * (1.9d; docs/kernel/CX_SMP.md).
 *
 * One AP at a time, as Haiku's loader (smp_boot_other_cpus) and XNU
 * (intel_startCPU, one start_cpu per CPU) do, rather than Serenity's one
 * INIT and STARTUP broadcast to all-but-self (APIC::do_boot_aps): a
 * broadcast wakes whatever answers, where the MADT names what exists, and
 * one at a time lets one trampoline page and one stack slot in it serve
 * every AP. For each enabled entry but the boot CPU's own: its struct cpu
 * and stacks, the trampoline's six words, INIT, 10 ms, STARTUP, 200 us, a
 * second STARTUP if it has not yet answered (an AP already running ignores
 * one), then a wait for it to park. One that never parks is sent INIT
 * again, which stops it wherever it got to, and is left out.
 */

#include "smp.h"
#include "idt.h"
#include "arch/smp.h"
#include "arch/paging.h"
#include "arch/cpu.h"
#include "arch/cpu_ops.h"
#include "arch/x86/apic.h"
#include "arch/x86/madt.h"
#include "boot/cxbi.h"
#include "memman/physmap.h"
#include "kstack.h"
#include "timer.h"
#include "logging.h"
#include "string.h"

#define AP_IDLE_STACK    16384u
#define AP_RSP0_STACK    8192u
#define INIT_WAIT_NS     10000000ull      /* 10 ms, the SDM's */
#define STARTUP_WAIT_NS  200000ull        /* 200 us */
#define PARK_WAIT_NS     1000000000ull    /* 1 s: a few thousand instructions */

extern uint8_t smp_tramp_start[], smp_tramp_end[];
extern uint8_t smp_tramp_cr0[], smp_tramp_cr3[], smp_tramp_cr4[];
extern uint8_t smp_tramp_stack[], smp_tramp_cpu[], smp_tramp_entry[];

static struct smp_ap_stacks stacks[CPU_MAX];
static volatile uint32_t    timer_hits[CPU_MAX];
static paddr_t              boot_root;

static void wait_ns(uint64_t ns) {
    uint64_t end = timer_now_ns() + ns;
    while (timer_now_ns() < end) arch_cpu_relax();
}

/* ---- on the AP ----------------------------------------------------------- */

/* Halted with interrupts on until something is asked of it. The check and
   the halt are one step: interrupts are off from the check to `sti; hlt`,
   and sti holds them off for one more instruction, so a wake IPI sent after
   the check is taken in the halt and ends it, never lost before it. */
static __attribute__((noreturn)) void park(struct cpu *c) {
    for (;;) {
        arch_irq_disable();
        void (*fn)(void *) = __atomic_load_n(&c->call, __ATOMIC_ACQUIRE);
        if (fn) {
            fn(c->call_arg);
            __atomic_store_n(&c->call, (void (*)(void *))0, __ATOMIC_RELAXED);
            __atomic_fetch_add(&c->calls_done, 1, __ATOMIC_RELEASE);
            continue;
        }
        arch_cpu_idle();          /* sti; hlt */
        c->wakes++;
    }
}

void smp_ap_entry(struct cpu *c);
void smp_ap_entry(struct cpu *c) {
    addr_space_switch(addr_space_kernel());
    const struct smp_ap_stacks *s = &stacks[c->index];
    if (arch_cpu_init_ap(c, s->ist_base, s->rsp0_top)) arch_cpu_halt_forever();
    lapic_ap_init();
    __atomic_store_n(&c->state, CPU_PARKED, __ATOMIC_RELEASE);
    park(c);
}

void smp_ap_interrupt(uint32_t vector) {
    struct cpu *c = cpu_this();
    c->irqs++;
    if (vector == LAPIC_TIMER_VECTOR) timer_hits[c->index]++;
}

/* ---- on the boot CPU ----------------------------------------------------- */

static void free_stacks(struct smp_ap_stacks *s) {
    kstack_free(s->idle_base);
    kstack_free(s->rsp0_base);
    for (unsigned i = 0; i < IST_STACKS; i++) kstack_free(s->ist_base[i]);
    *s = (struct smp_ap_stacks){0};
}

static int alloc_stacks(struct smp_ap_stacks *s) {
    *s = (struct smp_ap_stacks){0};
    s->idle_top = kstack_alloc(AP_IDLE_STACK, 0, &s->idle_base);
    s->rsp0_top = kstack_alloc(AP_RSP0_STACK, 0, &s->rsp0_base);
    int ok = s->idle_top && s->rsp0_top;
    for (unsigned i = 0; i < IST_STACKS; i++) {
        uintptr_t base = 0;
        if (!kstack_alloc(GDT64_IST_STACK_SIZE, 0, &base)) ok = 0;
        s->ist_base[i] = base;
    }
    if (!ok) free_stacks(s);
    return ok ? 0 : -1;
}

/* A quadword of the trampoline page, through the physmap (one at a time,
   interrupts off between the map and the unmap). */
static void tramp_put(const uint8_t *field, uint64_t v) {
    uint32_t f = arch_irq_save();
    uint8_t *page = phys_map(SMP_TRAMP_PHYS);
    *(volatile uint64_t *)(page + (field - smp_tramp_start)) = v;
    phys_unmap(page);
    arch_irq_restore(f);
}

static int tramp_install(paddr_t root) {
    uint32_t len = (uint32_t)(smp_tramp_end - smp_tramp_start);
    if (len > PAGE_SIZE) return -1;
    uint32_t f = arch_irq_save();
    uint8_t *page = phys_map(SMP_TRAMP_PHYS);
    memcpy(page, smp_tramp_start, len);
    phys_unmap(page);
    arch_irq_restore(f);

    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    tramp_put(smp_tramp_cr0, cr0);
    tramp_put(smp_tramp_cr3, root);
    tramp_put(smp_tramp_cr4, cr4 & ~(1ull << 17));   /* PCIDE: only in long mode */
    tramp_put(smp_tramp_entry, (uint64_t)(uintptr_t)smp_ap_entry);
    return 0;
}

/* The page must be RAM the firmware gave to no one: usable in its map. */
static int tramp_page_usable(void) {
    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (e->type == CXBI_MEM_USABLE && e->base <= SMP_TRAMP_PHYS &&
            e->base + e->length >= SMP_TRAMP_PHYS + PAGE_SIZE) return 1;
    }
    return 0;
}

static int parked(struct cpu *c) {
    return __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == CPU_PARKED;
}

static int wait_parked(struct cpu *c, uint64_t ns) {
    uint64_t end = timer_now_ns() + ns;
    while (!parked(c)) {
        if (timer_now_ns() >= end) return 0;
        arch_cpu_relax();
    }
    return 1;
}

static int start_one(struct cpu *c) {
    struct smp_ap_stacks *s = &stacks[c->index];
    if (alloc_stacks(s)) {
        klog_u32("SMP", SEV_WARN, "no stacks for the CPU at APIC ID ", c->hw_id, LOG_COLOR_VALUE, "");
        return -1;
    }
    tramp_put(smp_tramp_stack, s->idle_top);
    tramp_put(smp_tramp_cpu, (uint64_t)(uintptr_t)c);
    __atomic_store_n(&c->state, CPU_STARTING, __ATOMIC_RELEASE);

    int sent = lapic_send_init(c->hw_id) == 0;
    wait_ns(INIT_WAIT_NS);
    sent = sent && lapic_send_startup(c->hw_id, SMP_TRAMP_PHYS >> 12) == 0;
    wait_ns(STARTUP_WAIT_NS);
    if (sent && !parked(c)) sent = lapic_send_startup(c->hw_id, SMP_TRAMP_PHYS >> 12) == 0;
    if (sent && wait_parked(c, PARK_WAIT_NS)) return 0;

    /* Never parked: INIT puts it back to waiting for a STARTUP, wherever in
       the trampoline it got to, before its stacks go. */
    (void)lapic_send_init(c->hw_id);
    wait_ns(INIT_WAIT_NS);
    c->state = CPU_OFF;
    free_stacks(s);
    klog_u32("SMP", SEV_WARN, sent ? "no answer from the CPU at APIC ID "
                                   : "an IPI was not accepted by APIC ID ",
             c->hw_id, LOG_COLOR_VALUE, "");
    return -1;
}

unsigned arch_smp_start(void) {
    cpu_boot.hw_id = lapic_id();
    cpu_boot.state = CPU_RUNNING;

    const struct madt_cpu *list;
    const struct madt_cpu_counts *seen;
    uint32_t n = apic_cpus(&list, &seen);
    if (!lapic_active() || n <= 1) {
        klog("SMP", SEV_INFO, n <= 1 ? "one CPU listed: nothing to start"
                                     : "no local APIC: one CPU");
        return cpu_count();
    }
    if (!tramp_page_usable() || paging_boot_root(SMP_TRAMP_PHYS, &boot_root) ||
        tramp_install(boot_root)) {
        klog("SMP", SEV_WARN, "no room to start the other CPUs: one CPU");
        if (boot_root) { addr_space_release(boot_root); boot_root = 0; }
        return cpu_count();
    }

    int boot_listed = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (list[i].apic_id == cpu_boot.hw_id) { boot_listed = 1; continue; }
        struct cpu *c = cpu_next_slot();
        if (!c) { klog("SMP", SEV_WARN, "more CPUs listed than CPU_MAX: the rest stay off"); break; }
        c->hw_id = list[i].apic_id;
        if (start_one(c) == 0) cpu_join(c);
    }

    addr_space_release(boot_root);
    boot_root = 0;

    if (!boot_listed)
        klog_u32("SMP", SEV_WARN, "the boot CPU's APIC ID is not in the MADT: ",
                 cpu_boot.hw_id, LOG_COLOR_VALUE, "");
    klog_u32("SMP", SEV_OK, "CPUs running, the others parked: ", cpu_count(), LOG_COLOR_VALUE, "");
    klog_child_u32("listed enabled", seen->enabled, LOG_COLOR_VALUE, "");
    klog_child_u32("listed disabled", seen->disabled, LOG_COLOR_VALUE, "");
    return cpu_count();
}

int smp_call(unsigned index, void (*fn)(void *), void *arg, uint64_t timeout_ns) {
    struct cpu *c = cpu_at(index);
    if (!c || c == &cpu_boot || !parked(c) || !fn) return -1;
    if (__atomic_load_n(&c->call, __ATOMIC_ACQUIRE)) return -1;   /* one still pending */
    uint32_t done = __atomic_load_n(&c->calls_done, __ATOMIC_ACQUIRE);
    c->call_arg = arg;
    __atomic_store_n(&c->call, fn, __ATOMIC_RELEASE);
    if (lapic_send_fixed(c->hw_id, SMP_IPI_VECTOR)) return -1;
    uint64_t end = timer_now_ns() + timeout_ns;
    while (__atomic_load_n(&c->calls_done, __ATOMIC_ACQUIRE) == done) {
        if (timer_now_ns() >= end) return -1;
        arch_cpu_relax();
    }
    return 0;
}

const struct smp_ap_stacks *smp_stacks_of(unsigned index) {
    return index < CPU_MAX ? &stacks[index] : 0;
}

uint32_t smp_timer_hits(unsigned index) { return index < CPU_MAX ? timer_hits[index] : 0; }

