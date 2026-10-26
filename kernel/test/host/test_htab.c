// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_htab.c */
/* Aurora Tejeda / CATX Systems */
/*
 * object/htab.c: handle tables. The properties htab.h promises: a closed
 * handle's number is refused, 0 and small numbers are never handles, rights
 * only shrink, every handle has a unique grant id and records its parent, and
 * whatever happens the objects' live count returns to where it started. And
 * the table grows: no value changes when it does, it stops at its limit, and
 * a growth that cannot get memory refuses one handle and keeps the rest.
 */
#include "khost.h"
#include "tobj.h"
#include "object/htab.h"

#include <stdlib.h>
#include <string.h>

#define T_SMALL 16    /* one first array: full without growing */
#define T_RAND  40    /* grows 16 -> 32 -> 40 */

static int fail_allocs;   /* the next allocation fails while set */
static int live_arrays;   /* allocated and not yet freed */

void *htab_mem_alloc(uint32_t bytes) {
    if (fail_allocs) return 0;
    live_arrays++;
    return malloc(bytes);
}

void htab_mem_free(void *p) {
    live_arrays--;
    free(p);
}

static struct tobj *new_tobj(struct tobj *o) {
    tobj_type();
    memset(o, 0, sizeof *o);
    KH_CHECK(kobj_init(&o->hdr, KOBJ_TEST) == 0);
    return o;
}

KH_TEST(htab_values_are_never_zero_or_small_and_never_repeat) {
    struct htab t;
    struct tobj o;
    htab_init(&t, T_SMALL);
    new_tobj(&o);

    int32_t seen[T_SMALL * 4];
    int n = 0;
    for (int round = 0; round < 4; round++) {
        int32_t hs[T_SMALL];
        for (int i = 0; i < T_SMALL; i++) {
            hs[i] = htab_install(&t, &o.hdr, TOBJ_R1, 0);
            KH_CHECK(hs[i] >= (1 << HTAB_SLOT_BITS));
            for (int j = 0; j < n; j++) KH_CHECK(seen[j] != hs[i]);
            seen[n++] = hs[i];
        }
        for (int i = 0; i < T_SMALL; i++) KH_CHECK_EQ(htab_close(&t, hs[i]), 0);
    }
    kobj_unref(&o.hdr);
    KH_CHECK_EQ(o.destroy_runs, 1);
}

KH_TEST(htab_a_closed_number_is_refused_even_after_its_slot_is_reused) {
    struct htab t;
    struct tobj a, b;
    htab_init(&t, T_SMALL);
    new_tobj(&a);
    new_tobj(&b);

    int32_t ha = htab_install(&t, &a.hdr, TOBJ_R1, 0);
    KH_CHECK(ha > 0);
    KH_CHECK_EQ(htab_close(&t, ha), 0);
    KH_CHECK(htab_ref(&t, ha, KOBJ_NONE, 0, 0) == 0);
    KH_CHECK_EQ(htab_close(&t, ha), HTAB_E_BAD_HANDLE);
    KH_CHECK_EQ(htab_dup(&t, ha, 0), HTAB_E_BAD_HANDLE);

    int32_t hb = htab_install(&t, &b.hdr, TOBJ_R1, 0);     /* the same slot, first free */
    KH_CHECK(hb > 0 && hb != ha);
    KH_CHECK_EQ(hb & ((1 << HTAB_SLOT_BITS) - 1), ha & ((1 << HTAB_SLOT_BITS) - 1));
    KH_CHECK(htab_ref(&t, ha, KOBJ_NONE, 0, 0) == 0);     /* not b */
    KH_CHECK_EQ(htab_close(&t, ha), HTAB_E_BAD_HANDLE);
    KH_CHECK_EQ(b.last_handle_runs, 0);                     /* b untouched */

    struct kobj *r = htab_ref(&t, hb, KOBJ_TEST, 0, 0);
    KH_CHECK(r == &b.hdr);
    if (r) kobj_unref(r);

    htab_close_all(&t);
    kobj_unref(&a.hdr);
    kobj_unref(&b.hdr);
    KH_CHECK_EQ(a.destroy_runs, 1);
    KH_CHECK_EQ(b.destroy_runs, 1);
}

KH_TEST(htab_refuses_values_that_name_no_slot) {
    struct htab t;
    struct tobj o;
    htab_init(&t, T_SMALL);
    new_tobj(&o);
    int32_t h = htab_install(&t, &o.hdr, TOBJ_R1, 0);
    int32_t slot = h & ((1 << HTAB_SLOT_BITS) - 1);
    int32_t gen  = h >> HTAB_SLOT_BITS;

    const int32_t bad[] = {
        0, -1, HTAB_E_BAD_HANDLE, INT32_MIN, slot,                       /* no generation */
        ((gen + 1) << HTAB_SLOT_BITS) | slot,                 /* a future generation */
        (gen << HTAB_SLOT_BITS) | (int32_t)t.cap,             /* past the table */
        (gen << HTAB_SLOT_BITS) | ((1 << HTAB_SLOT_BITS) - 1),
        (gen << HTAB_SLOT_BITS) | (slot + 1),                 /* an empty slot */
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        KH_CHECK(htab_ref(&t, bad[i], KOBJ_NONE, 0, 0) == 0);
        KH_CHECK_EQ(htab_close(&t, bad[i]), HTAB_E_BAD_HANDLE);
        KH_CHECK_EQ(htab_dup(&t, bad[i], 0), HTAB_E_BAD_HANDLE);
    }
    KH_CHECK_EQ(htab_count(&t), 1u);
    KH_CHECK_EQ(htab_close(&t, h), 0);
    kobj_unref(&o.hdr);
    KH_CHECK_EQ(o.destroy_runs, 1);
}

KH_TEST(htab_rights_only_shrink) {
    struct htab t;
    struct tobj o;
    htab_init(&t, T_SMALL);
    new_tobj(&o);
    uint32_t live0 = kobj_live(KOBJ_TEST);

    /* A right the type does not define cannot be put on a handle at all. */
    KH_CHECK_EQ(htab_install(&t, &o.hdr, 0x8u, 0), HTAB_E_ACCESS_DENIED);
    KH_CHECK_EQ(htab_install(&t, &o.hdr, TOBJ_RIGHTS | 0x40000000u, 0), HTAB_E_ACCESS_DENIED);   /* no such generic right */
    KH_CHECK_EQ(htab_count(&t), 0u);
    KH_CHECK_EQ(atomic_load(&o.hdr.handles), 0u);

    int32_t h = htab_install(&t, &o.hdr, TOBJ_R1 | TOBJ_R2 | HTAB_RIGHT_DUPLICATE, 0);
    KH_CHECK(h > 0);

    /* A duplicate may keep or drop rights, never gain one, including one the
       type defines. */
    KH_CHECK_EQ(htab_dup(&t, h, TOBJ_R3), HTAB_E_ACCESS_DENIED);
    KH_CHECK_EQ(htab_dup(&t, h, TOBJ_R1 | TOBJ_R3), HTAB_E_ACCESS_DENIED);
    KH_CHECK_EQ(htab_dup(&t, h, 0x8u), HTAB_E_ACCESS_DENIED);
    int32_t d = htab_dup(&t, h, TOBJ_R2 | HTAB_RIGHT_DUPLICATE);
    KH_CHECK(d > 0 && d != h);
    KH_CHECK_EQ(htab_dup(&t, d, TOBJ_R1), HTAB_E_ACCESS_DENIED);          /* from the narrower one */

    /* need is checked against the handle, not the object */
    struct htab_info info;
    struct kobj *r = htab_ref(&t, d, KOBJ_TEST, TOBJ_R1, 0);
    KH_CHECK(r == 0);
    r = htab_ref(&t, d, KOBJ_TEST, TOBJ_R2, &info);
    KH_CHECK(r == &o.hdr);
    KH_CHECK_EQ(info.rights, TOBJ_R2 | HTAB_RIGHT_DUPLICATE);
    if (r) kobj_unref(r);
    r = htab_ref(&t, h, KOBJ_TEST, TOBJ_R1 | TOBJ_R2, 0);
    KH_CHECK(r == &o.hdr);
    if (r) kobj_unref(r);

    /* and the type has to match */
    KH_CHECK(htab_ref(&t, h, KOBJ_ENDPOINT, 0, 0) == 0);
    KH_CHECK(htab_ref(&t, h, KOBJ_FILE, 0, 0) == 0);

    KH_CHECK_EQ(atomic_load(&o.hdr.handles), 2u);
    htab_close_all(&t);
    KH_CHECK_EQ(o.last_handle_runs, 1);
    kobj_unref(&o.hdr);
    KH_CHECK_EQ(o.destroy_runs, 1);
    KH_CHECK_EQ(kobj_live(KOBJ_TEST), live0 - 1);
}

KH_TEST(htab_grants_are_unique_and_record_their_parent) {
    struct htab t, u;
    struct tobj o;
    htab_init(&t, T_SMALL);
    htab_init(&u, T_SMALL);
    new_tobj(&o);

    int32_t h = htab_install(&t, &o.hdr, TOBJ_RIGHTS | HTAB_RIGHT_DUPLICATE, 0);
    int32_t d = htab_dup(&t, h, TOBJ_R1 | HTAB_RIGHT_DUPLICATE);
    int32_t dd = htab_dup(&t, d, TOBJ_R1);

    struct htab_info ih, id, idd, iu;
    struct kobj *r;
    r = htab_ref(&t, h, KOBJ_NONE, 0, &ih);   KH_CHECK(r != 0); if (r) kobj_unref(r);
    r = htab_ref(&t, d, KOBJ_NONE, 0, &id);   KH_CHECK(r != 0); if (r) kobj_unref(r);
    r = htab_ref(&t, dd, KOBJ_NONE, 0, &idd); KH_CHECK(r != 0); if (r) kobj_unref(r);

    KH_CHECK(ih.grant != 0 && id.grant != 0 && idd.grant != 0);
    KH_CHECK(ih.grant != id.grant && id.grant != idd.grant && ih.grant != idd.grant);
    KH_CHECK_EQ(ih.parent, 0u);
    KH_CHECK_EQ(id.parent, ih.grant);
    KH_CHECK_EQ(idd.parent, id.grant);

    /* Across tables, as spawn hands a child a handle on its broker. */
    int32_t hu = htab_install(&u, &o.hdr, TOBJ_R2, ih.grant);
    r = htab_ref(&u, hu, KOBJ_NONE, 0, &iu);  KH_CHECK(r != 0); if (r) kobj_unref(r);
    KH_CHECK_EQ(iu.parent, ih.grant);
    KH_CHECK(iu.grant != ih.grant && iu.grant != id.grant && iu.grant != idd.grant);

    /* A grant id is never reused, even for the same slot. */
    KH_CHECK_EQ(htab_close(&t, dd), 0);
    int32_t again = htab_dup(&t, d, TOBJ_R1);
    struct htab_info ia;
    r = htab_ref(&t, again, KOBJ_NONE, 0, &ia); KH_CHECK(r != 0); if (r) kobj_unref(r);
    KH_CHECK(ia.grant != idd.grant);

    htab_close_all(&t);
    htab_close_all(&u);
    kobj_unref(&o.hdr);
    KH_CHECK_EQ(o.destroy_runs, 1);
}

KH_TEST(htab_a_full_table_refuses_and_takes_nothing) {
    struct htab t;
    struct tobj o;
    htab_init(&t, T_SMALL);
    new_tobj(&o);
    for (int i = 0; i < T_SMALL; i++) KH_CHECK(htab_install(&t, &o.hdr, TOBJ_R1, 0) > 0);
    uint32_t refs = atomic_load(&o.hdr.refs);
    KH_CHECK_EQ(htab_install(&t, &o.hdr, TOBJ_R1, 0), HTAB_E_NO_RESOURCES);
    KH_CHECK_EQ(htab_dup(&t, htab_install(&t, &o.hdr, TOBJ_R1, 0), 0), HTAB_E_BAD_HANDLE);
    KH_CHECK_EQ(atomic_load(&o.hdr.refs), refs);
    KH_CHECK_EQ(atomic_load(&o.hdr.handles), (uint32_t)T_SMALL);
    htab_close_all(&t);
    KH_CHECK_EQ(htab_count(&t), 0u);
    KH_CHECK_EQ(o.last_handle_runs, 1);
    kobj_unref(&o.hdr);
    KH_CHECK_EQ(o.destroy_runs, 1);
}

/* A lookup's reference is the caller's: closing the handle meanwhile makes the
   object unreachable but must not free it under the caller. */
KH_TEST(htab_a_looked_up_object_outlives_its_handle) {
    struct htab t;
    struct tobj o;
    htab_init(&t, T_SMALL);
    new_tobj(&o);
    int32_t h = htab_install(&t, &o.hdr, TOBJ_R1, 0);
    kobj_unref(&o.hdr);                         /* the creator lets go */

    struct kobj *r = htab_ref(&t, h, KOBJ_TEST, 0, 0);
    KH_CHECK(r == &o.hdr);
    KH_CHECK_EQ(htab_close(&t, h), 0);
    KH_CHECK_EQ(o.last_handle_runs, 1);
    KH_CHECK_EQ(o.destroy_runs, 0);
    if (r) kobj_unref(r);
    KH_CHECK_EQ(o.destroy_runs, 1);
}

/* White box: the generation wraps to 1, never 0, and values stay positive. */
KH_TEST(htab_generation_wraps_to_one_and_stays_positive) {
    struct htab t;
    struct tobj o;
    htab_init(&t, T_SMALL);
    new_tobj(&o);
    KH_CHECK_EQ(htab_close(&t, htab_install(&t, &o.hdr, TOBJ_R1, 0)), 0);   /* make the array */
    t.e[0].gen = HTAB_GEN_MAX;
    int32_t h = htab_install(&t, &o.hdr, TOBJ_R1, 0);
    KH_CHECK(h > 0);
    KH_CHECK_EQ((uint32_t)h >> HTAB_SLOT_BITS, HTAB_GEN_MAX);
    KH_CHECK_EQ(htab_close(&t, h), 0);
    KH_CHECK_EQ(t.e[0].gen, 1u);
    int32_t h2 = htab_install(&t, &o.hdr, TOBJ_R1, 0);
    KH_CHECK(h2 >= (1 << HTAB_SLOT_BITS) && h2 != h);
    KH_CHECK(htab_ref(&t, h, KOBJ_NONE, 0, 0) == 0);
    htab_close_all(&t);
    kobj_unref(&o.hdr);
}

/* Random installs, duplicates, lookups and closes over several objects,
   against a model of which values are open and with what rights. Every
   closed value stays refused, rights never grow, and the live count returns
   to where it started. */
KH_TEST(htab_random_operations_agree_with_a_model) {
    tobj_type();
    enum { N = 4, ROUNDS = 200, STEPS = 200, CLOSED_MAX = 512 };
    uint64_t seed = 0xC0FFEEull;
    uint32_t live0 = kobj_live(KOBJ_TEST);
    int p0 = tobj_panics;
    int a0 = live_arrays;

    for (int round = 0; round < ROUNDS; round++) {
        struct htab t;
        struct tobj o[N];
        htab_init(&t, T_RAND);
        for (int i = 0; i < N; i++) new_tobj(&o[i]);

        int32_t  val[T_RAND];
        int      obj[T_RAND];
        uint32_t rts[T_RAND];
        int      open = 0;
        int32_t  closed[CLOSED_MAX];
        int      nclosed = 0;

        for (int step = 0; step < STEPS; step++) {
            uint32_t op = kh_rand(&seed) % 5;
            uint32_t rights = kh_rand(&seed) & 0xFu;           /* sometimes invalid */
            if (kh_rand(&seed) & 1) rights |= HTAB_RIGHT_DUPLICATE;
            if (op == 0 || open == 0) {                        /* install */
                int i = (int)(kh_rand(&seed) % N);
                int32_t h = htab_install(&t, &o[i].hdr, rights, 0);
                if (rights & ~(TOBJ_RIGHTS | HTAB_RIGHT_DUPLICATE)) KH_CHECK_EQ(h, HTAB_E_ACCESS_DENIED);
                else if (open == T_RAND) KH_CHECK_EQ(h, HTAB_E_NO_RESOURCES);
                else { KH_CHECK(h > 0); val[open] = h; obj[open] = i; rts[open] = rights; open++; }
            } else {
                int k = (int)(kh_rand(&seed) % (uint32_t)open);
                if (op == 1) {                                 /* duplicate */
                    int32_t d = htab_dup(&t, val[k], rights);
                    if (!(rts[k] & HTAB_RIGHT_DUPLICATE)) KH_CHECK_EQ(d, HTAB_E_ACCESS_DENIED);
                    else if (rights & ~rts[k])   KH_CHECK_EQ(d, HTAB_E_ACCESS_DENIED);
                    else if (open == T_RAND) KH_CHECK_EQ(d, HTAB_E_NO_RESOURCES);
                    else { KH_CHECK(d > 0); val[open] = d; obj[open] = obj[k]; rts[open] = rights; open++; }
                } else if (op == 2) {                          /* look up */
                    struct htab_info info;
                    struct kobj *r = htab_ref(&t, val[k], KOBJ_TEST, rights & TOBJ_RIGHTS, &info);
                    if ((rights & TOBJ_RIGHTS & ~rts[k]) != 0) KH_CHECK(r == 0);
                    else {
                        KH_CHECK(r == &o[obj[k]].hdr);
                        KH_CHECK_EQ(info.rights, rts[k]);
                        if (r) kobj_unref(r);
                    }
                } else {                                       /* close */
                    KH_CHECK_EQ(htab_close(&t, val[k]), 0);
                    if (nclosed < CLOSED_MAX) closed[nclosed++] = val[k];
                    open--;
                    val[k] = val[open]; obj[k] = obj[open]; rts[k] = rts[open];
                }
            }
            KH_CHECK_EQ(htab_count(&t), (uint32_t)open);
            if (nclosed) {
                int32_t c = closed[kh_rand(&seed) % (uint32_t)nclosed];
                KH_CHECK(htab_ref(&t, c, KOBJ_NONE, 0, 0) == 0);
            }
        }

        for (int i = 0; i < N; i++) {
            int handles = 0;
            for (int k = 0; k < open; k++) handles += obj[k] == i;
            KH_CHECK_EQ(atomic_load(&o[i].hdr.handles), (uint32_t)handles);
        }
        htab_close_all(&t);
        for (int i = 0; i < nclosed; i++) KH_CHECK_EQ(htab_close(&t, closed[i]), HTAB_E_BAD_HANDLE);
        htab_destroy(&t);
        KH_CHECK_EQ(live_arrays, a0);
        for (int i = 0; i < N; i++) {
            kobj_unref(&o[i].hdr);
            KH_CHECK_EQ(o[i].destroy_runs, 1);
        }
    }
    KH_CHECK_EQ(kobj_live(KOBJ_TEST), live0);
    KH_CHECK_EQ(tobj_panics, p0);
}

/* Only a handle holding the duplicate right can be copied, and a copy holds
   it only if it was asked for. */
KH_TEST(htab_duplicate_needs_the_right) {
    struct htab t;
    struct tobj o;
    htab_init(&t, T_SMALL);
    new_tobj(&o);

    int32_t plain = htab_install(&t, &o.hdr, TOBJ_R1, 0);
    KH_CHECK(plain > 0);
    KH_CHECK_EQ(htab_dup(&t, plain, TOBJ_R1), HTAB_E_ACCESS_DENIED);
    KH_CHECK_EQ(htab_dup(&t, plain, 0), HTAB_E_ACCESS_DENIED);
    KH_CHECK_EQ(htab_dup(&t, plain, TOBJ_R1 | HTAB_RIGHT_DUPLICATE), HTAB_E_ACCESS_DENIED);

    int32_t h = htab_install(&t, &o.hdr, TOBJ_R1 | HTAB_RIGHT_DUPLICATE, 0);
    int32_t once = htab_dup(&t, h, TOBJ_R1);                         /* not asked for */
    KH_CHECK(once > 0);
    KH_CHECK_EQ(htab_dup(&t, once, TOBJ_R1), HTAB_E_ACCESS_DENIED);
    int32_t again = htab_dup(&t, h, TOBJ_R1 | HTAB_RIGHT_DUPLICATE); /* asked for */
    KH_CHECK(again > 0);
    int32_t third = htab_dup(&t, again, 0);
    KH_CHECK(third > 0);

    struct htab_info info;
    struct kobj *r = htab_ref(&t, once, KOBJ_NONE, 0, &info);
    KH_CHECK(r != 0); if (r) kobj_unref(r);
    KH_CHECK_EQ(info.rights, TOBJ_R1);
    KH_CHECK_EQ(htab_count(&t), 5u);
    KH_CHECK_EQ(atomic_load(&o.hdr.handles), 5u);

    htab_close_all(&t);
    kobj_unref(&o.hdr);
    KH_CHECK_EQ(o.destroy_runs, 1);
}

/* Growing moves no handle: every value made before a growth still names its
   object after several, and the array is freed with the table. */
KH_TEST(htab_grows_and_keeps_every_value) {
    int a0 = live_arrays;
    enum { N = 300 };
    struct htab t;
    struct tobj o[3];
    htab_init(&t, HTAB_LIMIT_DEFAULT);
    KH_CHECK(t.cap == 0 && t.e == 0);                       /* nothing until used */
    for (int i = 0; i < 3; i++) new_tobj(&o[i]);

    static int32_t h[N];
    for (int i = 0; i < N; i++) {
        h[i] = htab_install(&t, &o[i % 3].hdr, TOBJ_R1, 0);
        KH_CHECK(h[i] >= (1 << HTAB_SLOT_BITS));
        KH_CHECK_EQ(h[i] & (HTAB_MAX_SLOTS - 1), i);       /* lowest free slot */
    }
    KH_CHECK(t.cap >= N && t.cap < 2 * N);
    KH_CHECK_EQ(live_arrays, a0 + 1);                            /* the old ones went */
    for (int i = 0; i < N; i++) {
        struct kobj *r = htab_ref(&t, h[i], KOBJ_TEST, TOBJ_R1, 0);
        KH_CHECK(r == &o[i % 3].hdr);
        if (r) kobj_unref(r);
    }
    KH_CHECK_EQ(htab_count(&t), (uint32_t)N);
    htab_close_all(&t);
    KH_CHECK_EQ(htab_count(&t), 0u);
    htab_destroy(&t);
    KH_CHECK_EQ(live_arrays, a0);
    for (int i = 0; i < 3; i++) {
        KH_CHECK_EQ(atomic_load(&o[i].hdr.handles), 0u);
        kobj_unref(&o[i].hdr);
        KH_CHECK_EQ(o[i].destroy_runs, 1);
    }
}

/* A limit that is not a power of two is kept exactly, by install and by
   duplicate alike, and a refusal takes no reference. A limit past what a
   value can encode is cut to HTAB_MAX_SLOTS, where the last slot still makes
   a positive value. */
KH_TEST(htab_stops_at_its_limit) {
    int a0 = live_arrays;
    struct htab t;
    struct tobj o;
    htab_init(&t, T_RAND);
    new_tobj(&o);
    int32_t h = 0;
    for (int i = 0; i < T_RAND; i++) {
        h = htab_install(&t, &o.hdr, TOBJ_R1 | HTAB_RIGHT_DUPLICATE, 0);
        KH_CHECK(h > 0);
    }
    uint32_t refs = atomic_load(&o.hdr.refs);
    KH_CHECK_EQ(htab_install(&t, &o.hdr, TOBJ_R1, 0), HTAB_E_NO_RESOURCES);
    KH_CHECK_EQ(htab_dup(&t, h, TOBJ_R1), HTAB_E_NO_RESOURCES);
    KH_CHECK_EQ(atomic_load(&o.hdr.refs), refs);
    KH_CHECK_EQ(t.cap, (uint32_t)T_RAND);
    htab_close_all(&t);
    htab_destroy(&t);

    htab_init(&t, 0xFFFFFFFFu);
    KH_CHECK_EQ(t.max, HTAB_MAX_SLOTS);
    for (uint32_t i = 0; i < HTAB_MAX_SLOTS; i++) {
        h = htab_install(&t, &o.hdr, TOBJ_R1, 0);
        KH_CHECK(h > 0);
    }
    KH_CHECK_EQ(h & (HTAB_MAX_SLOTS - 1), (int32_t)(HTAB_MAX_SLOTS - 1));
    struct kobj *r = htab_ref(&t, h, KOBJ_TEST, 0, 0);
    KH_CHECK(r == &o.hdr);
    if (r) kobj_unref(r);
    KH_CHECK_EQ(htab_install(&t, &o.hdr, TOBJ_R1, 0), HTAB_E_NO_RESOURCES);
    htab_close_all(&t);
    htab_destroy(&t);
    KH_CHECK_EQ(live_arrays, a0);
    kobj_unref(&o.hdr);
    KH_CHECK_EQ(o.destroy_runs, 1);
}

/* No memory to grow: the one handle is refused, with no reference taken, and
   every handle already there still works. Memory back, it grows. */
KH_TEST(htab_growth_failure_refuses_and_keeps) {
    int a0 = live_arrays;
    struct htab t;
    struct tobj o;
    htab_init(&t, HTAB_LIMIT_DEFAULT);
    new_tobj(&o);
    int32_t h[HTAB_FIRST_SLOTS];
    for (int i = 0; i < HTAB_FIRST_SLOTS; i++)
        h[i] = htab_install(&t, &o.hdr, TOBJ_R1 | HTAB_RIGHT_DUPLICATE, 0);

    uint32_t refs = atomic_load(&o.hdr.refs);
    fail_allocs = 1;
    KH_CHECK_EQ(htab_install(&t, &o.hdr, TOBJ_R1, 0), HTAB_E_NO_RESOURCES);
    KH_CHECK_EQ(htab_dup(&t, h[0], TOBJ_R1), HTAB_E_NO_RESOURCES);
    fail_allocs = 0;
    KH_CHECK_EQ(atomic_load(&o.hdr.refs), refs);
    KH_CHECK_EQ(t.cap, (uint32_t)HTAB_FIRST_SLOTS);
    for (int i = 0; i < HTAB_FIRST_SLOTS; i++) {
        struct kobj *r = htab_ref(&t, h[i], KOBJ_TEST, TOBJ_R1, 0);
        KH_CHECK(r == &o.hdr);
        if (r) kobj_unref(r);
    }
    int32_t d = htab_dup(&t, h[0], TOBJ_R1);
    KH_CHECK(d > 0);
    KH_CHECK_EQ(d & (HTAB_MAX_SLOTS - 1), HTAB_FIRST_SLOTS);
    htab_close_all(&t);
    htab_destroy(&t);
    KH_CHECK_EQ(live_arrays, a0);
    kobj_unref(&o.hdr);
    KH_CHECK_EQ(o.destroy_runs, 1);
}
