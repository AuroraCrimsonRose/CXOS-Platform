// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/percpu.c */
/* Aurora Tejeda / CATX Systems */
/* The boot CPU's own data (percpu.h). The entry stub binds it before kmain
   runs, so even the first spinlock finds it. */

#include <stddef.h>
#include "percpu.h"

struct cpu cpu_boot = { .self = &cpu_boot };

_Static_assert(offsetof(struct cpu, self)     == CPU_SELF,     "the entry stubs read it here");
_Static_assert(offsetof(struct cpu, kstack)   == CPU_KSTACK,   "the syscall entry reads it here");
_Static_assert(offsetof(struct cpu, user_rsp) == CPU_USER_RSP, "the syscall entry parks it here");
_Static_assert(offsetof(struct cpu, thread)   == CPU_THREAD,   "cpu_thread reads it here");
_Static_assert(offsetof(struct cpu, spin_held) == CPU_SPIN_HELD, "cpu_spin_held reads it here");

/* The application processors' (1.9d), handed out in index order. */
static struct cpu  cpu_ap[CPU_MAX - 1];
static struct cpu *cpu_table[CPU_MAX] = { &cpu_boot };
static unsigned    cpu_online = 1;

struct cpu *cpu_at(unsigned index) { return index < cpu_online ? cpu_table[index] : 0; }

unsigned cpu_count(void) { return cpu_online; }

struct cpu *cpu_next_slot(void) {
    if (cpu_online >= CPU_MAX) return 0;
    struct cpu *c = &cpu_ap[cpu_online - 1];
    *c = (struct cpu){ .self = c, .index = cpu_online };
    return c;
}

void cpu_join(struct cpu *c) {
    if (c->index != cpu_online || c != &cpu_ap[cpu_online - 1]) return;
    cpu_table[cpu_online++] = c;
}
