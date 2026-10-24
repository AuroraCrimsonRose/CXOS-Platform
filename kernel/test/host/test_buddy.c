// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_buddy.c */
/* Aurora Tejeda / CATX Systems */
/*
 * memman/buddy.c: the page database and the buddy allocator over it.
 *
 * A small database stands in for physical memory: 4096 pages, the zone
 * boundary at 2048 (it must be a multiple of the largest block, as 4 GB is in
 * the kernel). The random test checks every allocation against a shadow array
 * that cannot be wrong in the same way: one byte per page, set while handed
 * out.
 */
#include "khost.h"
#include "memman/buddy.h"

#include <string.h>

#define NPAGES   4096u
#define LOW_END  2048u
#define MAXBLK   (1u << BUDDY_MAX_ORDER)

static struct page db[NPAGES];
static struct buddy b;

static void fresh(void) {
    KH_CHECK_EQ(buddy_init(&b, db, NPAGES, LOW_END), 0);
}

KH_TEST(buddy_zone_boundary_must_align_to_the_largest_block) {
    KH_CHECK(buddy_init(&b, db, NPAGES, LOW_END + 1) == BUDDY_E_RANGE);
    KH_CHECK(buddy_init(&b, db, NPAGES, MAXBLK / 2) == BUDDY_E_RANGE);
    KH_CHECK_EQ(buddy_init(&b, db, NPAGES, MAXBLK * 3), 0);
}

KH_TEST(buddy_starts_with_nothing_free) {
    fresh();
    KH_CHECK_EQ(buddy_free_count(&b), 0);
    KH_CHECK_EQ(buddy_alloc(&b, 0, 0), BUDDY_NONE);
    for (uint32_t i = 0; i < NPAGES; i++) KH_CHECK_EQ(db[i].state, PG_RESERVED);
}

/* Each page is added once, however often a range names it; pages past the
   database are ignored. */
KH_TEST(buddy_add_counts_each_page_once) {
    fresh();
    KH_CHECK_EQ(buddy_add(&b, 100, 50), 50);
    KH_CHECK_EQ(buddy_add(&b, 120, 50), 20);
    KH_CHECK_EQ(buddy_add(&b, 100, 70), 0);
    KH_CHECK_EQ(buddy_add(&b, NPAGES - 10, 100), 10);
    KH_CHECK_EQ(buddy_add(&b, NPAGES, 5), 0);
    KH_CHECK_EQ(buddy_free_count(&b), 80);
    KH_CHECK_EQ(b.managed, 80);
}

/* A block of order k starts on a multiple of 2^k and lies inside what was
   added. */
KH_TEST(buddy_blocks_are_aligned_to_their_size) {
    fresh();
    buddy_add(&b, 3, NPAGES - 3);
    for (unsigned k = 0; k <= BUDDY_MAX_ORDER; k++) {
        uint32_t pfn = buddy_alloc(&b, k, 0);
        KH_CHECK(pfn != BUDDY_NONE);
        KH_CHECK_EQ(pfn & ((1u << k) - 1), 0);
        KH_CHECK(pfn >= 3 && pfn + (1u << k) <= NPAGES);
        for (uint32_t i = 0; i < (1u << k); i++) KH_CHECK_EQ(db[pfn + i].state, PG_USED);
    }
}

/* Freeing everything merges it back: the whole database is then two blocks
   of the largest order per zone, and nothing smaller. */
KH_TEST(buddy_free_merges_back_to_the_largest_blocks) {
    fresh();
    buddy_add(&b, 0, NPAGES);
    uint32_t got[NPAGES];
    for (uint32_t i = 0; i < NPAGES; i++) got[i] = buddy_alloc(&b, 0, 0);
    KH_CHECK_EQ(buddy_alloc(&b, 0, 0), BUDDY_NONE);
    KH_CHECK_EQ(buddy_free_count(&b), 0);
    /* in an order that is not the order of allocation */
    for (uint32_t i = 0; i < NPAGES; i += 2) KH_CHECK_EQ(buddy_free(&b, got[i], 1), 0);
    for (uint32_t i = 1; i < NPAGES; i += 2) KH_CHECK_EQ(buddy_free(&b, got[i], 1), 0);
    KH_CHECK_EQ(buddy_free_count(&b), NPAGES);
    for (unsigned z = 0; z < BUDDY_ZONES; z++)
        for (unsigned k = 0; k < BUDDY_MAX_ORDER; k++)
            KH_CHECK_EQ(b.head[z][k], BUDDY_NONE);
    for (unsigned i = 0; i < NPAGES / MAXBLK; i++)
        KH_CHECK(buddy_alloc(&b, BUDDY_MAX_ORDER, 0) != BUDDY_NONE);
}

/* A page freed twice, a page never handed out, and a range with one free page
   in it are all refused, and nothing in the range is freed. */
KH_TEST(buddy_double_free_is_refused) {
    fresh();
    buddy_add(&b, 0, 64);
    uint32_t p = buddy_alloc(&b, 2, 0);
    uint32_t free_before = buddy_free_count(&b);
    KH_CHECK_EQ(buddy_free(&b, p, 4), 0);
    KH_CHECK(buddy_free(&b, p, 4) == BUDDY_E_NOT_USED);
    KH_CHECK(buddy_free(&b, p + 1, 1) == BUDDY_E_NOT_USED);
    KH_CHECK_EQ(buddy_free_count(&b), free_before + 4);

    KH_CHECK(buddy_free(&b, 100, 1) == BUDDY_E_NOT_USED);   /* reserved */
    KH_CHECK(buddy_free(&b, NPAGES, 1) == BUDDY_E_RANGE);
    KH_CHECK(buddy_free(&b, NPAGES - 1, 2) == BUDDY_E_RANGE);

    uint32_t q = buddy_alloc(&b, 3, 0);
    KH_CHECK_EQ(buddy_free(&b, q + 7, 1), 0);
    uint32_t mid = buddy_free_count(&b);
    KH_CHECK(buddy_free(&b, q, 8) == BUDDY_E_NOT_USED);
    KH_CHECK_EQ(buddy_free_count(&b), mid);
    for (uint32_t i = 0; i < 7; i++) KH_CHECK_EQ(db[q + i].state, PG_USED);
}

/* The high zone serves first; the low zone is asked for by name, and saying
   so never yields a high page, even when only high pages are left. */
KH_TEST(buddy_zones) {
    fresh();
    buddy_add(&b, 0, NPAGES);
    uint32_t p = buddy_alloc(&b, 0, 0);
    KH_CHECK(p >= LOW_END);
    uint32_t q = buddy_alloc(&b, 4, 1);
    KH_CHECK(q + 16 <= LOW_END);
    KH_CHECK_EQ(b.free_pages[BUDDY_ZONE_LOW], LOW_END - 16);
    KH_CHECK_EQ(b.free_pages[BUDDY_ZONE_HIGH], NPAGES - LOW_END - 1);

    while (buddy_alloc(&b, 0, 1) != BUDDY_NONE) { }
    KH_CHECK_EQ(b.free_pages[BUDDY_ZONE_LOW], 0);
    KH_CHECK_EQ(buddy_alloc(&b, 0, 1), BUDDY_NONE);
    KH_CHECK(buddy_alloc(&b, 0, 0) >= LOW_END);
}

/* When the high zone is empty an ordinary request falls back to the low one. */
KH_TEST(buddy_falls_back_to_the_low_zone) {
    fresh();
    buddy_add(&b, 10, 20);
    uint32_t p = buddy_alloc(&b, 0, 0);
    KH_CHECK(p >= 10 && p < 30);
}

/* Freed pages on both sides of the boundary never merge across it. */
KH_TEST(buddy_no_block_straddles_the_zones) {
    fresh();
    buddy_add(&b, LOW_END - MAXBLK, 2 * MAXBLK);
    uint32_t lo = buddy_alloc(&b, BUDDY_MAX_ORDER, 1);
    uint32_t hi = buddy_alloc(&b, BUDDY_MAX_ORDER, 0);
    KH_CHECK_EQ(lo, LOW_END - MAXBLK);
    KH_CHECK_EQ(hi, LOW_END);
    KH_CHECK_EQ(buddy_free(&b, lo, MAXBLK), 0);
    KH_CHECK_EQ(buddy_free(&b, hi, MAXBLK), 0);
    KH_CHECK_EQ(b.head[BUDDY_ZONE_LOW][BUDDY_MAX_ORDER], LOW_END - MAXBLK);
    KH_CHECK_EQ(b.head[BUDDY_ZONE_HIGH][BUDDY_MAX_ORDER], LOW_END);
    KH_CHECK_EQ(db[LOW_END].order, BUDDY_MAX_ORDER);
}

KH_TEST(buddy_order_for_counts) {
    KH_CHECK_EQ(buddy_order_for(0), BUDDY_ORDERS);
    KH_CHECK_EQ(buddy_order_for(1), 0);
    KH_CHECK_EQ(buddy_order_for(2), 1);
    KH_CHECK_EQ(buddy_order_for(3), 2);
    KH_CHECK_EQ(buddy_order_for(MAXBLK), BUDDY_MAX_ORDER);
    KH_CHECK_EQ(buddy_order_for(MAXBLK + 1), BUDDY_ORDERS);
}

/* Random allocations and frees, of random orders and in part, against a
   shadow: no page is handed out twice, every block lies in RAM, the free count
   agrees, and once everything is freed it all merges back. */
KH_TEST(buddy_random_against_a_shadow) {
    static uint8_t shadow[NPAGES];
    static struct { uint32_t pfn, n; } live[NPAGES];
    uint64_t seed = 0x5C5C5C5Cull;
    fresh();
    memset(shadow, 0, sizeof shadow);
    /* RAM with holes, as a firmware map has */
    buddy_add(&b, 1, 700);
    buddy_add(&b, 900, 1500);
    buddy_add(&b, 2600, NPAGES - 2600);
    uint32_t total = buddy_free_count(&b), used = 0, nlive = 0;
    for (int step = 0; step < 20000; step++) {
        if (nlive == 0 || kh_rand(&seed) % 3 != 0) {
            unsigned k = kh_rand(&seed) % (BUDDY_MAX_ORDER + 1);
            int low = (kh_rand(&seed) % 4) == 0;
            uint32_t pfn = buddy_alloc(&b, k, low);
            if (pfn == BUDDY_NONE) continue;
            KH_CHECK(!low || pfn + (1u << k) <= LOW_END);
            for (uint32_t i = 0; i < (1u << k); i++) {
                uint32_t q = pfn + i;
                KH_CHECK(q >= 1 && !(q >= 701 && q < 900) && !(q >= 2400 && q < 2600));
                KH_CHECK_EQ(shadow[q], 0);
                shadow[q] = 1;
            }
            live[nlive].pfn = pfn;
            live[nlive].n = 1u << k;
            nlive++;
            used += 1u << k;
        } else {
            uint32_t i = kh_rand(&seed) % nlive;
            /* free a part from the front, keep the rest live */
            uint32_t n = 1 + kh_rand(&seed) % live[i].n;
            KH_CHECK_EQ(buddy_free(&b, live[i].pfn, n), 0);
            for (uint32_t j = 0; j < n; j++) shadow[live[i].pfn + j] = 0;
            used -= n;
            live[i].pfn += n;
            live[i].n -= n;
            if (live[i].n == 0) live[i] = live[--nlive];
        }
        KH_CHECK_EQ(buddy_free_count(&b), total - used);
    }
    for (uint32_t i = 0; i < nlive; i++) KH_CHECK_EQ(buddy_free(&b, live[i].pfn, live[i].n), 0);
    KH_CHECK_EQ(buddy_free_count(&b), total);
    /* every free page heads a block or is a tail of one, and the heads cover
       exactly the free pages */
    uint32_t covered = 0;
    for (unsigned z = 0; z < BUDDY_ZONES; z++)
        for (unsigned k = 0; k <= BUDDY_MAX_ORDER; k++)
            for (uint32_t p = b.head[z][k]; p != BUDDY_NONE; p = db[p].next) {
                KH_CHECK_EQ(db[p].order, k);
                covered += 1u << k;
            }
    KH_CHECK_EQ(covered, total);
}
