// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_vmtree.c */
/* Aurora Tejeda / CATX Systems */
/*
 * memman/vmtree.c: the region tree.
 *
 * The random test runs thousands of inserts and removes against a shadow
 * that cannot be wrong in the same way, a flat array of slots each either
 * taken or free, and after each change checks the tree's order, its balance
 * and every answer it gives (find, overlap, first fit) against the shadow.
 */
#include "khost.h"
#include "memman/vmtree.h"

#define SLOTS 256u      /* unit-sized slots; a node covers a run of them */
#define UNIT  0x1000ull
#define LO    0x10000000ull

static struct vm_node nodes[SLOTS];
static int8_t owner[SLOTS];          /* the node covering each slot, or -1 */
static uint8_t used[SLOTS];          /* node i is in the tree */

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(uint32_t n) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng % n);
}

/* Ordered, balanced, heights right; returns the height, or -1. */
static int check(const struct vm_node *n, uint64_t lo, uint64_t hi) {
    if (!n) return 0;
    if (n->base < lo || n->base + n->len > hi) return -1;
    int l = check(n->left, lo, n->base);
    int r = check(n->right, n->base + n->len, hi);
    if (l < 0 || r < 0) return -1;
    if (l - r > 1 || r - l > 1) return -1;
    if (n->height != (l > r ? l : r) + 1) return -1;
    return n->height;
}

/* The shadow's first fit of `units` slots. */
static uint64_t shadow_fit(uint32_t units) {
    for (uint32_t s = 0; s + units <= SLOTS; s++) {
        uint32_t k = 0;
        while (k < units && owner[s + k] < 0) k++;
        if (k == units) return LO + s * UNIT;
        s += k;
    }
    return 0;
}

KH_TEST(vmtree_empty_tree_answers_nothing) {
    struct vm_node *root = 0;
    KH_CHECK(vmtree_find(root, LO) == 0);
    KH_CHECK(vmtree_overlap(root, LO, UNIT) == 0);
    KH_CHECK_EQ(vmtree_first_fit(root, LO, LO + 4 * UNIT, UNIT), LO);
    KH_CHECK_EQ(vmtree_first_fit(root, LO, LO + 4 * UNIT, 5 * UNIT), 0);
    KH_CHECK_EQ(vmtree_count(root), 0);
}

KH_TEST(vmtree_refuses_an_overlap_and_leaves_the_tree_alone) {
    struct vm_node *root = 0;
    struct vm_node a = { .base = LO + 2 * UNIT, .len = 2 * UNIT };
    struct vm_node b = { .base = LO + 3 * UNIT, .len = UNIT };
    struct vm_node c = { .base = LO + UNIT,     .len = 2 * UNIT };
    struct vm_node d = { .base = LO + 4 * UNIT, .len = UNIT };   /* touches, no overlap */
    KH_CHECK_EQ(vmtree_insert(&root, &a), 0);
    KH_CHECK_EQ(vmtree_insert(&root, &b), -1);
    KH_CHECK_EQ(vmtree_insert(&root, &c), -1);
    KH_CHECK_EQ(vmtree_insert(&root, &d), 0);
    KH_CHECK_EQ(vmtree_count(root), 2);
    KH_CHECK(vmtree_find(root, LO + 4 * UNIT - 1) == &a);
    KH_CHECK(vmtree_find(root, LO + 4 * UNIT) == &d);
    KH_CHECK(vmtree_find(root, LO + 2 * UNIT - 1) == 0);
    /* a fit between, before and after */
    KH_CHECK_EQ(vmtree_first_fit(root, LO, LO + 8 * UNIT, 2 * UNIT), LO);
    KH_CHECK_EQ(vmtree_first_fit(root, LO + UNIT, LO + 8 * UNIT, 2 * UNIT), LO + 5 * UNIT);
    KH_CHECK_EQ(vmtree_first_fit(root, LO + UNIT, LO + 7 * UNIT, 2 * UNIT), LO + 5 * UNIT);
    KH_CHECK_EQ(vmtree_first_fit(root, LO + UNIT, LO + 6 * UNIT, 2 * UNIT), 0);
}

/* A run of ascending inserts is the case an unbalanced tree turns into a
   list: the height must stay logarithmic. */
KH_TEST(vmtree_stays_balanced_under_ascending_inserts) {
    struct vm_node *root = 0;
    for (uint32_t i = 0; i < SLOTS; i++) {
        nodes[i].base = LO + i * UNIT;
        nodes[i].len  = UNIT;
        KH_CHECK_EQ(vmtree_insert(&root, &nodes[i]), 0);
    }
    KH_CHECK_EQ(vmtree_count(root), SLOTS);
    KH_CHECK(check(root, 0, ~0ull) > 0);
    KH_CHECK(vmtree_height(root) <= 11);          /* 1.45 log2(258) */
    KH_CHECK_EQ(vmtree_first_fit(root, LO, LO + SLOTS * UNIT, UNIT), 0);
    for (uint32_t i = 0; i < SLOTS; i += 2) vmtree_remove(&root, &nodes[i]);
    KH_CHECK_EQ(vmtree_count(root), SLOTS / 2);
    KH_CHECK(check(root, 0, ~0ull) > 0);
    KH_CHECK_EQ(vmtree_first_fit(root, LO, LO + SLOTS * UNIT, UNIT), LO);
    KH_CHECK_EQ(vmtree_first_fit(root, LO, LO + SLOTS * UNIT, 2 * UNIT), 0);
}

KH_TEST(vmtree_random_against_a_shadow) {
    struct vm_node *root = 0;
    for (uint32_t i = 0; i < SLOTS; i++) { owner[i] = -1; used[i] = 0; }
    uint32_t live = 0;

    for (int step = 0; step < 20000; step++) {
        uint32_t i = rnd(128);                    /* node index */
        if (used[i]) {
            vmtree_remove(&root, &nodes[i]);
            uint32_t s = (uint32_t)((nodes[i].base - LO) / UNIT);
            for (uint32_t k = 0; k < nodes[i].len / UNIT; k++) owner[s + k] = -1;
            used[i] = 0;
            live--;
        } else {
            uint32_t s = rnd(SLOTS), units = 1 + rnd(4);
            if (s + units > SLOTS) units = SLOTS - s;
            int clear = 1;
            for (uint32_t k = 0; k < units; k++) if (owner[s + k] >= 0) clear = 0;
            nodes[i].base = LO + s * UNIT;
            nodes[i].len  = units * UNIT;
            int rc = vmtree_insert(&root, &nodes[i]);
            KH_CHECK_EQ(rc, clear ? 0 : -1);
            if (rc == 0) {
                for (uint32_t k = 0; k < units; k++) owner[s + k] = (int8_t)i;
                used[i] = 1;
                live++;
            }
        }
        if (step % 50 == 0 || step > 19900) {
            KH_CHECK(check(root, 0, ~0ull) >= 0);
            KH_CHECK_EQ(vmtree_count(root), live);
            for (uint32_t s = 0; s < SLOTS; s++) {
                struct vm_node *f = vmtree_find(root, LO + s * UNIT + 7);
                KH_CHECK(owner[s] < 0 ? f == 0 : f == &nodes[owner[s]]);
                KH_CHECK((vmtree_overlap(root, LO + s * UNIT, UNIT) != 0) == (owner[s] >= 0));
            }
            for (uint32_t u = 1; u <= 6; u++)
                KH_CHECK_EQ(vmtree_first_fit(root, LO, LO + SLOTS * UNIT, u * UNIT), shadow_fit(u));
        }
    }
}
