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
