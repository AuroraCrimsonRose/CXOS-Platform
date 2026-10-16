// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_idtab.c */
/* Aurora Tejeda / CATX Systems */
/* Id tables (object/idtab.h): lowest free id, growth, the ceiling, and a
   growth that cannot get memory. */
#include "khost.h"
#include "object/idtab.h"
#include <stdlib.h>

static int fail_allocs;   /* the next allocation fails while set */
static int live_arrays;   /* allocated and not yet freed */

void *idtab_mem_alloc(uint32_t bytes) {
    if (fail_allocs) return 0;
    live_arrays++;
    return malloc(bytes);
}

void idtab_mem_free(void *p) {
    live_arrays--;
    free(p);
}

static int obj[1024];

KH_TEST(idtab_lowest_free_id) {
    struct idtab t;
    idtab_init(&t, 64);
    KH_CHECK_EQ(t.cap, 0);
    KH_CHECK(idtab_get(&t, 0) == 0);
    KH_CHECK_EQ(idtab_alloc(&t, &obj[0], 0), 0);
    KH_CHECK_EQ(idtab_alloc(&t, &obj[1], 0), 1);
    KH_CHECK_EQ(idtab_alloc(&t, &obj[2], 0), 2);
    KH_CHECK(idtab_get(&t, 1) == &obj[1]);
    KH_CHECK(idtab_take(&t, 1) == &obj[1]);
    KH_CHECK(idtab_get(&t, 1) == 0);
    KH_CHECK(idtab_take(&t, 1) == 0);
    KH_CHECK_EQ(t.used, 2);
    KH_CHECK_EQ(idtab_alloc(&t, &obj[3], 0), 1);       /* the hole first */
    KH_CHECK_EQ(idtab_alloc(&t, &obj[4], 5), 5);       /* the floor is kept */
    KH_CHECK_EQ(idtab_alloc(&t, &obj[5], 1), 3);
    KH_CHECK(idtab_alloc(&t, 0, 0) == -1);             /* NULL names nothing */
    KH_CHECK(idtab_get(&t, -1) == 0);
    KH_CHECK(idtab_get(&t, 1 << 30) == 0);
    idtab_destroy(&t);
    KH_CHECK_EQ(live_arrays, 0);
}

KH_TEST(idtab_grows_past_first_array) {
    struct idtab t;
    idtab_init(&t, 1000);
    for (int i = 0; i < 300; i++) KH_CHECK_EQ(idtab_alloc(&t, &obj[i], 0), i);
    KH_CHECK(t.cap >= 300);
    KH_CHECK(t.grows > 1);
    KH_CHECK_EQ(live_arrays, 1);                       /* old arrays freed */
    for (int i = 0; i < 300; i++) KH_CHECK(idtab_get(&t, i) == &obj[i]);   /* copied */
    for (int i = 0; i < 300; i++) KH_CHECK(idtab_take(&t, i) == &obj[i]);
    KH_CHECK_EQ(t.used, 0);
    idtab_destroy(&t);
    KH_CHECK_EQ(live_arrays, 0);
}

KH_TEST(idtab_ceiling_is_kept) {
    struct idtab t;
    idtab_init(&t, 40);                                /* not a power of two */
    for (int i = 0; i < 40; i++) KH_CHECK_EQ(idtab_alloc(&t, &obj[i], 0), i);
    KH_CHECK_EQ(t.cap, 40);
    KH_CHECK(idtab_alloc(&t, &obj[40], 0) == -1);
    KH_CHECK(idtab_alloc(&t, &obj[40], 40) == -1);
    KH_CHECK(idtab_take(&t, 17) == &obj[17]);
    KH_CHECK_EQ(idtab_alloc(&t, &obj[40], 0), 17);
    idtab_destroy(&t);
}

KH_TEST(idtab_growth_failure_refuses_and_keeps) {
    struct idtab t;
    idtab_init(&t, 1000);
    for (int i = 0; i < IDTAB_FIRST_CAP; i++) KH_CHECK_EQ(idtab_alloc(&t, &obj[i], 0), i);
    fail_allocs = 1;
    KH_CHECK(idtab_alloc(&t, &obj[100], 0) == -1);
    fail_allocs = 0;
    KH_CHECK_EQ(t.cap, IDTAB_FIRST_CAP);
    for (int i = 0; i < IDTAB_FIRST_CAP; i++) KH_CHECK(idtab_get(&t, i) == &obj[i]);
    KH_CHECK_EQ(idtab_alloc(&t, &obj[100], 0), IDTAB_FIRST_CAP);
    for (int i = 0; i <= IDTAB_FIRST_CAP; i++) idtab_take(&t, i);
    idtab_destroy(&t);
    KH_CHECK_EQ(live_arrays, 0);
}
