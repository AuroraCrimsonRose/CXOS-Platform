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

struct cpu {
    struct cpu *self;          /* CPU_SELF: what the base register points at */
    uintptr_t   kstack;        /* CPU_KSTACK: the syscall entry's stack, the
                                  running thread's esp0 (arch_kernel_stack_set) */
    uintptr_t   user_rsp;      /* CPU_USER_RSP: ring 3's RSP, parked by the
                                  syscall entry for two instructions */
    uint32_t    index;         /* 0 for the boot CPU */
    int32_t     thread;        /* CPU_THREAD: the running thread's id (sched.c) */
    volatile uint32_t spin_held;   /* CPU_SPIN_HELD: spinlocks held (sync/spinlock.h, test hooks) */
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

/* How many spinlocks this CPU holds, the same way: a thread that holds none
   may move CPUs mid-read, one that holds one cannot. */
static inline uint32_t cpu_spin_held(void) { return arch_cpu_local_u32(CPU_SPIN_HELD); }

#endif
