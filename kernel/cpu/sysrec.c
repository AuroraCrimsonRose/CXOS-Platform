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

void sysrec_enter(uint32_t num, uint32_t a1, uint32_t a2) {
    (void)num; (void)a1; (void)a2;   /* what a recorder will keep */
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
}
