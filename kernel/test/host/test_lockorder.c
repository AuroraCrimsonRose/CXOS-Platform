// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_lockorder.c */
/* Aurora Tejeda / CATX Systems */
/* The lock order's checker (sync/lockorder.h): a lock is taken only above
   every rank held, the held list follows takes and gives in any order, and
   each nesting is recorded against the highest rank held. */
#include "khost.h"
#include "sync/lockorder.h"
#include <string.h>

static int A, B, C, D;   /* their addresses are the locks */

KH_TEST(lockorder_takes_in_order) {
    struct lockorder_held h = { 0 };
    uint32_t seen[LR_COUNT] = { 0 }, who = 99;
    KH_CHECK_EQ(lockorder_take(&h, seen, &A, LR_IPC, 1, &who), LO_OK);
    KH_CHECK_EQ(lockorder_take(&h, seen, &B, LR_HTAB, 2, &who), LO_OK);
    KH_CHECK_EQ(lockorder_take(&h, seen, &C, LR_KWAIT, 3, &who), LO_OK);
    KH_CHECK_EQ(h.n, 3u);
    KH_CHECK(h.e[2].lock == &C && h.e[2].rank == LR_KWAIT && h.e[2].at == 3);
    KH_CHECK_EQ(who, 99u);   /* untouched when all is well */
    /* each pair against the highest rank held, the first against none */
    KH_CHECK_EQ(seen[LR_NONE], 1u << LR_IPC);
    KH_CHECK_EQ(seen[LR_IPC],  1u << LR_HTAB);
    KH_CHECK_EQ(seen[LR_HTAB], 1u << LR_KWAIT);
    KH_CHECK_EQ(seen[LR_KWAIT], 0u);
}

KH_TEST(lockorder_refuses_lower_and_equal) {
    struct lockorder_held h = { 0 };
    uint32_t seen[LR_COUNT] = { 0 }, who = 99;
    KH_CHECK_EQ(lockorder_take(&h, seen, &A, LR_IPC, 1, &who), LO_OK);
    KH_CHECK_EQ(lockorder_take(&h, seen, &B, LR_KWAIT, 2, &who), LO_OK);
    uint32_t before[LR_COUNT]; memcpy(before, seen, sizeof seen);

    /* below the top held: names the entry in the way, adds nothing */
    KH_CHECK_EQ(lockorder_take(&h, seen, &C, LR_HTAB, 3, &who), LO_ORDER);
    KH_CHECK_EQ(who, 1u);
    /* below the bottom one too: the first in the way is named */
    KH_CHECK_EQ(lockorder_take(&h, seen, &C, LR_IPC, 3, &who), LO_ORDER);
    KH_CHECK_EQ(who, 0u);
    /* the same rank: two of one class are never held together */
    who = 99;
    KH_CHECK_EQ(lockorder_take(&h, seen, &C, LR_KWAIT, 3, &who), LO_ORDER);
    KH_CHECK_EQ(who, 1u);
    KH_CHECK_EQ(h.n, 2u);
    KH_CHECK(memcmp(before, seen, sizeof seen) == 0);

    /* one above the top is fine: the bound is exact */
    KH_CHECK_EQ(lockorder_take(&h, seen, &C, LR_KWAIT + 1, 4, &who), LO_OK);
}

KH_TEST(lockorder_refuses_unranked) {
    struct lockorder_held h = { 0 };
    uint32_t seen[LR_COUNT] = { 0 }, who = 0;
    KH_CHECK_EQ(lockorder_take(&h, seen, &A, LR_NONE, 1, &who), LO_UNRANKED);
    KH_CHECK_EQ(lockorder_take(&h, seen, &A, LR_COUNT, 1, &who), LO_UNRANKED);
    KH_CHECK_EQ(lockorder_take(&h, seen, &A, LR_COUNT - 1, 1, &who), LO_OK);
    KH_CHECK_EQ(h.n, 1u);
}

KH_TEST(lockorder_gives_in_any_order) {
    struct lockorder_held h = { 0 };
    uint32_t seen[LR_COUNT] = { 0 }, who = 0;
    lockorder_take(&h, seen, &A, LR_IPC, 1, &who);
    lockorder_take(&h, seen, &B, LR_HTAB, 2, &who);
    lockorder_take(&h, seen, &C, LR_KWAIT, 3, &who);
    lockorder_take(&h, seen, &D, LR_PAGEDB, 5, &who);
    KH_CHECK_EQ(lockorder_give(&h, &B), LO_OK);     /* from the middle */
    KH_CHECK_EQ(h.n, 3u);
    /* the rest keep the order they were taken in (the panic lists them so) */
    KH_CHECK(h.e[0].lock == &A && h.e[1].lock == &C && h.e[2].lock == &D);
    KH_CHECK(h.e[1].at == 3 && h.e[2].at == 5);
    KH_CHECK_EQ(lockorder_give(&h, &B), LO_NOT_HELD);
    KH_CHECK_EQ(lockorder_give(&h, &D), LO_OK);
    KH_CHECK_EQ(lockorder_give(&h, &D), LO_NOT_HELD);
    /* with htab given back, a lower rank than kwait is still refused */
    KH_CHECK_EQ(lockorder_take(&h, seen, &B, LR_HTAB, 4, &who), LO_ORDER);
    KH_CHECK_EQ(lockorder_give(&h, &C), LO_OK);
    KH_CHECK_EQ(lockorder_take(&h, seen, &B, LR_HTAB, 4, &who), LO_OK);
    KH_CHECK_EQ(lockorder_give(&h, &A), LO_OK);
    KH_CHECK_EQ(lockorder_give(&h, &B), LO_OK);
    KH_CHECK_EQ(h.n, 0u);
}

KH_TEST(lockorder_depth_is_bounded) {
    struct lockorder_held h = { 0 };
    uint32_t seen[LR_COUNT] = { 0 }, who = 0;
    static int locks[LOCKORDER_DEPTH + 1];
    for (uint32_t i = 0; i < LOCKORDER_DEPTH; i++)
        KH_CHECK_EQ(lockorder_take(&h, seen, &locks[i], LR_IPC + i, i, &who), LO_OK);
    KH_CHECK_EQ(lockorder_take(&h, seen, &locks[LOCKORDER_DEPTH], LR_IPC + LOCKORDER_DEPTH,
                               0, &who), LO_DEPTH);
    KH_CHECK_EQ(h.n, (uint32_t)LOCKORDER_DEPTH);
}

KH_TEST(lockorder_names_every_rank) {
    for (uint32_t r = 1; r < LR_COUNT; r++) {
        const char *n = lockorder_name(r);
        KH_CHECK(n && n[0] != '(');
        for (uint32_t q = 1; q < r; q++) KH_CHECK(strcmp(n, lockorder_name(q)) != 0);
    }
    KH_CHECK(strcmp(lockorder_name(LR_COUNT), "(bad rank)") == 0);
}
