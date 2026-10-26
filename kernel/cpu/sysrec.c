// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/sysrec.c */
/* Aurora Tejeda / CATX Systems */
/* The one recording point. See sysrec.h. */

#include "sysrec.h"
#include "sync/atomic.h"

static atomic_u32 entered;
static atomic_u32 exited;
static atomic_u32 last_num;
static atomic_u32 last_rc;
static atomic_u32 in_num;
static atomic_u32 in_args[SYSCALL_ARGS];

/* The last entry is kept whole, number and every register, so a test can see
   that the gate carried all of them; a recorder will keep more. */
void sysrec_enter(uint32_t num, const uint32_t args[SYSCALL_ARGS]) {
    atomic_store(&in_num, num);
    for (int i = 0; i < SYSCALL_ARGS; i++) atomic_store(&in_args[i], args[i]);
    atomic_fetch_add(&entered, 1);
}

void sysrec_exit(uint32_t num, int rc) {
    atomic_store(&last_num, num);
    atomic_store(&last_rc, (uint32_t)rc);
    atomic_fetch_add(&exited, 1);
}

void sysrec_snapshot(struct sysrec_stats *out) {
    out->entered  = atomic_load(&entered);
    out->exited   = atomic_load(&exited);
    out->last_num = atomic_load(&last_num);
    out->last_rc  = (int32_t)atomic_load(&last_rc);
    out->in_num   = atomic_load(&in_num);
    for (int i = 0; i < SYSCALL_ARGS; i++) out->in_args[i] = atomic_load(&in_args[i]);
}
