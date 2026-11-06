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
#include "sched.h"        /* 1.9e2: each AP's idle thread */

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

/* ---- the TLB, on any CPU (arch/smp.h) ------------------------------------ */

/* No page is global (pagetable.c sets no G bit), so a CR3 reload drops
   the kernel half's entries with the rest. */
static void tlb_flush_local(uintptr_t va, uint32_t pages) {
    if (pages == 0 || pages > SMP_TLB_FULL_AT) {
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
        return;
    }
    for (uint32_t i = 0; i < pages; i++)
        __asm__ volatile ("invlpg (%0)" : : "r"(va + (uintptr_t)i * PAGE_SIZE) : "memory");
}

/* Out of a halt: busy from here, and whatever a sender skipped while this
   CPU was idle is flushed before it touches anything that may have moved.
   The store, the fence and the read are the mirror of the sender's (stale,
   fence, idle): one of the two sees the other, so a request is either
   signalled or caught here, never missed. Until this runs, an idle CPU's
   interrupt path touches only what is never unmapped: the kernel image,
   its own stacks and struct cpu. */
static void tlb_wake(struct cpu *c) {
    __atomic_store_n(&c->idle, 0u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_exchange_n(&c->tlb_stale, 0u, __ATOMIC_ACQUIRE)) {
        tlb_flush_local(0, 0);
        c->tlb_woke_stale++;
    }
}

void smp_serve(void) {
    struct cpu *me = cpu_this();
    if (!__atomic_load_n(&me->tlb_from, __ATOMIC_RELAXED)) return;
    uint32_t from = __atomic_exchange_n(&me->tlb_from, 0u, __ATOMIC_ACQUIRE);
    uint32_t mine = 1u << me->index;
    while (from) {
        struct cpu *src = cpu_at((unsigned)__builtin_ctz(from));
        from &= from - 1;
        if (!src) continue;
        const struct cpu_tlb_req *r = &src->tlb_out;
        /* A space it has left since was flushed by the CR3 load. */
        if (r->root == SMP_TLB_KERNEL || r->root == me->space) tlb_flush_local(r->va, r->pages);
        me->tlb_served++;
        __atomic_fetch_and(&src->tlb_out.left, ~mine, __ATOMIC_RELEASE);
    }
}

/* One request per sender, in its own struct cpu, and a bit per sender in
   each target's tlb_from: WRK's per-processor request packets
   (KiIpiSendRequest, ke/amd64/flushtb.c) rather than one lock every
   shootdown queues on. Targets by the CR3 each CPU holds, as XNU's
   pmap_flush_tlbs reads cpu_active_cr3 (osfmk/x86_64/pmap.c), and an idle
   one marked stale rather than interrupted, as XNU leaves idle CPUs to
   "check as they go busy". The sender serves its own requests while it
   waits, as Serenity's smp_broadcast_wait_sync does (Processor.cpp). */
uint32_t smp_tlb_shootdown(uint64_t root, uintptr_t va, uint32_t pages, uint64_t timeout_ns) {
    unsigned n = cpu_count();
    if (n <= 1) return 0;
    uint32_t f = arch_irq_save();
    struct cpu *me = cpu_this();
    struct cpu_tlb_req *req = &me->tlb_out;
    req->root  = root;
    req->va    = va;
    req->pages = pages;
    /* The tables' new entries are stored before any CPU's space or idle is
       read: a CPU loading `root` after this walks the new tables. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t targets = 0;
    for (unsigned i = 0; i < n; i++) {
        struct cpu *c = cpu_at(i);
        if (c == me) continue;
        if (root != SMP_TLB_KERNEL && __atomic_load_n(&c->space, __ATOMIC_RELAXED) != root) continue;
        if (__atomic_load_n(&c->idle, __ATOMIC_RELAXED)) {
            __atomic_store_n(&c->tlb_stale, 1u, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (__atomic_load_n(&c->idle, __ATOMIC_RELAXED)) continue;   /* flushes as it wakes */
        }
        targets |= 1u << i;
    }
    __atomic_store_n(&req->left, targets, __ATOMIC_RELEASE);
    uint32_t bit = 1u << me->index;
    for (unsigned i = 0; i < n; i++) {
        if (!(targets & (1u << i))) continue;
        struct cpu *c = cpu_at(i);
        __atomic_fetch_or(&c->tlb_from, bit, __ATOMIC_RELEASE);
        (void)lapic_send_fixed(c->hw_id, SMP_IPI_VECTOR);   /* not accepted: it times out */
    }
    uint32_t left = targets;
    if (targets) {
        uint64_t end = timer_now_ns() + timeout_ns;
        while ((left = __atomic_load_n(&req->left, __ATOMIC_ACQUIRE)) != 0) {
            smp_serve();
            if (timer_now_ns() >= end) break;
            arch_cpu_relax();
        }
    }
    /* Never answered: withdrawn, so a late answer cannot clear a bit of
       the next request. (One already being served may still clear it; the
       paging layer panics on any of this.) */
    for (unsigned i = 0; left && i < n; i++)
        if (left & (1u << i)) __atomic_fetch_and(&cpu_at(i)->tlb_from, ~bit, __ATOMIC_RELAXED);
    arch_irq_restore(f);
    return left;
}

/* ---- on the AP ----------------------------------------------------------- */

/* The idle thread's halt (1.9e2; it was the park loop of 1.9d). Entered
   with interrupts off from the scheduler's look on: a call left meanwhile
   is run; otherwise idle, then `sti; hlt; cli`, and sti holds interrupts off
   for one more instruction, so an IPI sent after the look (a call, a
   thread handed to this CPU) is taken in the halt and ends it, never lost
   before it. Idle from just before the halt to tlb_wake: a shootdown
   meanwhile marks it stale rather than waking it. */
void smp_idle_wait(void) {
    struct cpu *c = cpu_this();
    void (*fn)(void *) = __atomic_load_n(&c->call, __ATOMIC_ACQUIRE);
    if (fn) {
        fn(c->call_arg);
        __atomic_store_n(&c->call, (void (*)(void *))0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&c->calls_done, 1, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&c->idle, 1u, __ATOMIC_RELAXED);
    arch_cpu_idle_irqs_off();     /* sti; hlt; cli */
    c->wakes++;
    tlb_wake(c);
}

void smp_ap_entry(struct cpu *c);
void smp_ap_entry(struct cpu *c) {
    addr_space_switch_on(c, addr_space_kernel());   /* GS is not this CPU's yet */
    const struct smp_ap_stacks *s = &stacks[c->index];
    if (arch_cpu_init_ap(c, s->ist_base, s->rsp0_top)) arch_cpu_halt_forever();
    lapic_ap_init();
    __atomic_store_n(&c->state, CPU_PARKED, __ATOMIC_RELEASE);
    /* Until the boot CPU has counted it in, no shootdown asks it: then
       everything it may hold is flushed, and it runs threads (1.9e2). */
    while (cpu_at(c->index) != c) arch_cpu_relax();
    tlb_flush_local(0, 0);
    __atomic_store_n(&c->state, CPU_RUNNING, __ATOMIC_RELEASE);
    sched_cpu_enter();
}

void smp_ap_interrupt(uint32_t vector) {
    struct cpu *c = cpu_this();
    c->irqs++;
    if (vector == LAPIC_TIMER_VECTOR) timer_hits[c->index]++;
    if (vector == SMP_IPI_VECTOR) smp_serve();
}

void smp_kick(unsigned index) {
    struct cpu *c = cpu_at(index);
    if (c && c != cpu_this()) (void)lapic_send_fixed(c->hw_id, SMP_IPI_VECTOR);
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
        /* Its idle thread first: the AP is that thread from its first
           instruction (sched.h). */
        if (sched_cpu_prepare(c) != 0) {
            klog_u32("SMP", SEV_WARN, "no idle thread for the CPU at APIC ID ", c->hw_id, LOG_COLOR_VALUE, "");
            continue;
        }
        if (start_one(c) == 0) cpu_join(c);
        else sched_cpu_unprepare(c);
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

/* What calls_done was when each CPU's call was left (smp_call_wait). */
static uint32_t call_done_before[CPU_MAX];

int smp_call_start(unsigned index, void (*fn)(void *), void *arg) {
    struct cpu *c = cpu_at(index);
    if (!c || c == &cpu_boot || __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != CPU_RUNNING || !fn)
        return -1;
    if (__atomic_load_n(&c->call, __ATOMIC_ACQUIRE)) return -1;   /* one still pending */
    call_done_before[index] = __atomic_load_n(&c->calls_done, __ATOMIC_ACQUIRE);
    c->call_arg = arg;
    __atomic_store_n(&c->call, fn, __ATOMIC_RELEASE);
    return lapic_send_fixed(c->hw_id, SMP_IPI_VECTOR) ? -1 : 0;
}

int smp_call_wait(unsigned index, uint64_t timeout_ns) {
    struct cpu *c = cpu_at(index);
    if (!c || c == &cpu_boot) return -1;
    uint64_t end = timer_now_ns() + timeout_ns;
    while (__atomic_load_n(&c->calls_done, __ATOMIC_ACQUIRE) == call_done_before[index]) {
        if (timer_now_ns() >= end) return -1;
        if (!arch_irqs_enabled()) smp_serve();   /* the call may be shooting down at us */
        arch_cpu_relax();
    }
    return 0;
}

int smp_call(unsigned index, void (*fn)(void *), void *arg, uint64_t timeout_ns) {
    if (smp_call_start(index, fn, arg)) return -1;
    return smp_call_wait(index, timeout_ns);
}

const struct smp_ap_stacks *smp_stacks_of(unsigned index) {
    return index < CPU_MAX ? &stacks[index] : 0;
}

uint32_t smp_timer_hits(unsigned index) { return index < CPU_MAX ? timer_hits[index] : 0; }

