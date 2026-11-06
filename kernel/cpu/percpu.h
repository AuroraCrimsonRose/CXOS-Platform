// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/percpu.h */
/* Aurora Tejeda / CATX Systems */
/*
 * What each CPU keeps of its own (1.9b): the thread it runs, the stack the
 * syscall entry moves to, the lock checker's state, and (1.9c) the frames
 * it has mapped.
 *
 * One struct cpu per CPU, found through the architecture's per-CPU base
 * register (on x86-64 the GS base, swapped in on every entry from ring 3:
 * arch_cpu_self, arch/x86/cpu_ops.h). It holds its own address, as WRK's
 * KPCR holds Self (base/ntos/inc/amd64.h) and XNU's cpu_data cpu_this
 * (osfmk/i386/cpu_data.h); here it is the first word, so the pointer is
 * one load at offset 0. The boot CPU's is cpu_boot, bound before
 * kmain runs; the others come with their CPUs (1.9d).
 *
 * The pointer holds only while the thread cannot move: with interrupts
 * off, or holding a spinlock. A field the running thread reads of itself
 * goes through cpu_thread(), a single load through the base register, so a
 * thread moved between CPUs mid-read cannot see another CPU's.
 *
 * The first three fields are read by the syscall entry
 * (arch/x86_64/usermode.asm): keep CPU_* and the struct in step.
 */
#ifndef CXK_PERCPU_H
#define CXK_PERCPU_H

#include <stdint.h>
#include "arch/cpu_ops.h"
#include "sync/lockorder.h"
#include "memman/physmap.h"

struct thread;

struct cpu {
    struct cpu *self;          /* CPU_SELF: what the base register points at */
    uintptr_t   kstack;        /* CPU_KSTACK: the syscall entry's stack, the
                                  running thread's esp0 (arch_kernel_stack_set) */
    uintptr_t   user_rsp;      /* CPU_USER_RSP: ring 3's RSP, parked by the
                                  syscall entry for two instructions */
    uint32_t    index;         /* 0 for the boot CPU */
    int32_t     thread;        /* CPU_THREAD: the running thread's id (sched.c) */
    volatile uint32_t spin_held;   /* CPU_SPIN_HELD: spinlocks held (sync/spinlock.h, test hooks) */
    struct thread *cur;        /* CPU_CUR (1.9e2): the running thread's record, so
                                  the running thread finds itself without the id
                                  table, which another CPU may be growing */
    struct lockorder_held locks;   /* what this CPU holds (sync/lockorder.c) */
    struct physmap_open   phys;    /* what this CPU has mapped (memman/physmap.c) */
    /* 1.9d: which CPU this is to the interrupt controller, and the parked
       CPU's mailbox (arch/smp.h) */
    uint32_t    hw_id;         /* its local APIC ID, as the MADT names it */
    volatile uint32_t state;   /* CPU_OFF, CPU_STARTING, CPU_PARKED, CPU_RUNNING */
    void (*volatile call)(void *);   /* work for a parked CPU: smp_call */
    void       *call_arg;
    volatile uint32_t calls_done;
    volatile uint32_t wakes;   /* times the park loop came out of a halt */
    volatile uint32_t irqs;    /* interrupts it took (none is routed to it) */
    /* 1.9e1: TLB shootdown (arch/smp.h). The root its CR3 holds, written
       before the load; the senders whose request it must serve, a bit per
       CPU index; its own request, one at a time; idle while halted with
       nothing to run, when a sender marks it stale instead of interrupting
       it, and it flushes everything as it wakes. */
    volatile uint64_t space;
    volatile uint32_t tlb_from;
    volatile uint32_t tlb_stale;
    volatile uint32_t idle;
    struct cpu_tlb_req {
        uint64_t root;             /* SMP_TLB_KERNEL: the half every space shares */
        uintptr_t va;
        uint32_t pages;            /* past SMP_TLB_FULL_AT: everything */
        volatile uint32_t left;    /* CPUs still to answer */
    } tlb_out;
    volatile uint32_t tlb_served;  /* requests it served, interrupted or spinning */
    volatile uint32_t tlb_woke_stale;  /* wakes that flushed for a skipped request */
    /* 1.9e2: the scheduler on this CPU (cpu/sched.c). Its idle thread, run
       when nothing else may; a switch asked for at its next preemption point
       (set by any CPU under sched_lock, with the IPI that ends a halt); the
       switch under way; when the running thread's slice ends; the IPIs it
       was sent for a switch. */
    struct thread    *idle_thread;
    volatile uint32_t need_resched;
    volatile uint32_t in_switch;
    volatile uint64_t slice_end;
    volatile uint32_t resched_ipis;
    /* 1.9e3: what the clock (timer_now_ns) takes off this CPU's TSC so it
       reads the boot CPU's: 0 there, and on an AP what bring-up measured
       (arch/smp.h, the TSC's agreement). Written only on this CPU. */
    volatile int64_t  tsc_adjust;
};

#define CPU_MAX      16        /* the rig has 8; Hyper-V is given up to 16 */
#define CPU_OFF      0
#define CPU_STARTING 1
#define CPU_PARKED   2
#define CPU_RUNNING  3         /* the boot CPU: running threads */

#define CPU_SELF     0
#define CPU_KSTACK   8
#define CPU_USER_RSP 16
#define CPU_THREAD   28
#define CPU_SPIN_HELD 32
#define CPU_CUR      40

extern struct cpu cpu_boot;

/* The CPUs running, the boot CPU first (index 0): cpu_at(i) for i below
   cpu_count(), 0 past it. A CPU joins once it has parked (arch/smp.h). */
struct cpu *cpu_at(unsigned index);
unsigned    cpu_count(void);

/* For the CPU starter: the next index's struct, cleared, with self and
   index set (0 when CPU_MAX are running); then cpu_join adds it, which
   only the next index may do. A start that failed leaves its slot to the
   next CPU tried. */
struct cpu *cpu_next_slot(void);
void        cpu_join(struct cpu *c);

static inline struct cpu *cpu_this(void) { return (struct cpu *)arch_cpu_self(); }

/* The running thread's id, in one load (see above). */
static inline int cpu_thread(void) { return (int)arch_cpu_local_u32(CPU_THREAD); }

/* The running thread's record, the same way (1.9e2). */
static inline struct thread *cpu_thread_rec(void) { return (struct thread *)arch_cpu_local_ptr(CPU_CUR); }
/* How many spinlocks this CPU holds, the same way: a thread that holds none
   may move CPUs mid-read, one that holds one cannot. */
static inline uint32_t cpu_spin_held(void) { return arch_cpu_local_u32(CPU_SPIN_HELD); }

#endif
