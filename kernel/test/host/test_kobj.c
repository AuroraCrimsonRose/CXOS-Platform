// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_kobj.c */
/* Aurora Tejeda / CATX Systems */
/*
 * object/kobj.c: object lifetimes. The properties every later type relies on:
 * an object is destroyed exactly once, never while referenced; the last
 * handle's hook runs exactly once, before destruction; a count never goes
 * below zero without being reported; and the live count returns to where it
 * started.
 */
#include "khost.h"
#include "object/kobj.h"

#include <string.h>

struct tobj {
    struct kobj hdr;
    int         last_handle_runs;
    int         destroy_runs;
};

static int panics;
void kobj_panic(const char *why) { (void)why; panics++; }

static void t_last_handle(struct kobj *o) { ((struct tobj *)o)->last_handle_runs++; }
static void t_destroy(struct kobj *o)     { ((struct tobj *)o)->destroy_runs++; }

static const struct kobj_ops test_ops = {
    .name = "test", .rights_valid = 0, .last_handle = t_last_handle, .destroy = t_destroy,
};

static void test_type(void) {
    if (!kobj_type_ops(KOBJ_TEST)) kobj_type_register(KOBJ_TEST, &test_ops);
}

KH_TEST(kobj_registration_refuses_what_could_not_work) {
    static const struct kobj_ops no_destroy = { .name = "x", .destroy = 0 };
    test_type();
    KH_CHECK(kobj_type_register(KOBJ_NONE, &test_ops) == -1);
    KH_CHECK(kobj_type_register(KOBJ_TYPE_MAX, &test_ops) == -1);
    KH_CHECK(kobj_type_register(KOBJ_ENDPOINT, &no_destroy) == -1);
    KH_CHECK(kobj_type_register(KOBJ_ENDPOINT, 0) == -1);
    KH_CHECK(kobj_type_register(KOBJ_TEST, &test_ops) == -1);   /* already */
}

KH_TEST(kobj_init_refuses_an_unregistered_type_and_counts_nothing) {
    struct tobj o;
    memset(&o, 0, sizeof o);
    uint32_t before = kobj_live(KOBJ_FILE);
    if (kobj_type_ops(KOBJ_FILE)) return;          /* registered elsewhere: nothing to check */
    KH_CHECK(kobj_init(&o.hdr, KOBJ_FILE) == -1);
    KH_CHECK_EQ(kobj_live(KOBJ_FILE), before);
}

KH_TEST(kobj_last_handle_then_destroy_each_exactly_once) {
    test_type();
    struct tobj o;
    memset(&o, 0, sizeof o);
    uint32_t live0 = kobj_live(KOBJ_TEST);

    KH_CHECK(kobj_init(&o.hdr, KOBJ_TEST) == 0);
    KH_CHECK_EQ(kobj_live(KOBJ_TEST), live0 + 1);
    kobj_handle_opened(&o.hdr);
    kobj_handle_opened(&o.hdr);
    kobj_unref(&o.hdr);                 /* the creator lets go; handles keep it */
    KH_CHECK_EQ(o.destroy_runs, 0);

    kobj_handle_closed(&o.hdr);
    KH_CHECK_EQ(o.last_handle_runs, 0);
    KH_CHECK_EQ(o.destroy_runs, 0);

    kobj_handle_closed(&o.hdr);
    KH_CHECK_EQ(o.last_handle_runs, 1);
    KH_CHECK_EQ(o.destroy_runs, 1);
    KH_CHECK_EQ(kobj_live(KOBJ_TEST), live0);
}

/* The endpoint case: a thread blocked on the object holds a reference, so the
   last handle closing makes it unreachable but must not free it. */
KH_TEST(kobj_a_reference_outlives_the_last_handle) {
    test_type();
    struct tobj o;
    memset(&o, 0, sizeof o);
    KH_CHECK(kobj_init(&o.hdr, KOBJ_TEST) == 0);
    kobj_handle_opened(&o.hdr);
    kobj_unref(&o.hdr);                 /* creator */

    kobj_ref(&o.hdr);                   /* a blocked thread */
    kobj_handle_closed(&o.hdr);
    KH_CHECK_EQ(o.last_handle_runs, 1);
    KH_CHECK_EQ(o.destroy_runs, 0);

    kobj_unref(&o.hdr);                 /* the thread wakes and lets go */
    KH_CHECK_EQ(o.destroy_runs, 1);
}

KH_TEST(kobj_counts_below_zero_are_reported_not_obeyed) {
    test_type();
    struct tobj o;
    memset(&o, 0, sizeof o);
    KH_CHECK(kobj_init(&o.hdr, KOBJ_TEST) == 0);
    kobj_unref(&o.hdr);
    KH_CHECK_EQ(o.destroy_runs, 1);

    int p0 = panics;
    kobj_unref(&o.hdr);                 /* a second release of a dead object */
    KH_CHECK_EQ(panics, p0 + 1);
    KH_CHECK_EQ(o.destroy_runs, 1);     /* not destroyed twice */

    kobj_handle_closed(&o.hdr);         /* a handle that was never opened */
    KH_CHECK_EQ(panics, p0 + 2);
    KH_CHECK_EQ(o.destroy_runs, 1);
}

/* Random interleavings of open, close, ref and unref on several objects,
   against a model: nothing destroyed early, everything destroyed once, and the
   live count back where it started. */
KH_TEST(kobj_random_lifetimes_agree_with_a_model) {
    test_type();
    enum { N = 8, ROUNDS = 500 };
    uint64_t seed = 0xB0B0ull;
    uint32_t live0 = kobj_live(KOBJ_TEST);
    int p0 = panics;

    for (int round = 0; round < ROUNDS; round++) {
        struct tobj o[N];
        int handles[N], extra[N];
        memset(o, 0, sizeof o);
        for (int i = 0; i < N; i++) {
            KH_CHECK(kobj_init(&o[i].hdr, KOBJ_TEST) == 0);
            kobj_handle_opened(&o[i].hdr);
            kobj_unref(&o[i].hdr);
            handles[i] = 1;
            extra[i] = 0;
        }

        for (int step = 0; step < 64; step++) {
            int i = (int)(kh_rand(&seed) % N);
            if (handles[i] == 0 && extra[i] == 0) continue;   /* destroyed */
            switch (kh_rand(&seed) % 4) {
            case 0: if (handles[i]) { kobj_handle_opened(&o[i].hdr); handles[i]++; } break;
            case 1: if (handles[i]) { kobj_handle_closed(&o[i].hdr); handles[i]--; } break;
            case 2: kobj_ref(&o[i].hdr); extra[i]++; break;
            case 3: if (extra[i])   { kobj_unref(&o[i].hdr); extra[i]--; } break;
            }
            int alive = handles[i] + extra[i] > 0;
            KH_CHECK_EQ(o[i].destroy_runs, alive ? 0 : 1);
            KH_CHECK_EQ(o[i].last_handle_runs, handles[i] ? 0 : 1);
        }

        for (int i = 0; i < N; i++) {           /* tear everything down */
            while (handles[i]) { kobj_handle_closed(&o[i].hdr); handles[i]--; }
            while (extra[i])   { kobj_unref(&o[i].hdr); extra[i]--; }
            KH_CHECK_EQ(o[i].destroy_runs, 1);
            KH_CHECK_EQ(o[i].last_handle_runs, 1);
        }
    }
    KH_CHECK_EQ(kobj_live(KOBJ_TEST), live0);
    KH_CHECK_EQ(panics, p0);
}
