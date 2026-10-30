// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/vmtree.c */
/* Aurora Tejeda / CATX Systems */
/* The region tree: see vmtree.h. Recursion is bounded by the height, which
   an AVL tree keeps under 1.45 log2(n + 2): 13 levels for the 256 regions a
   process may hold. */
#include "memman/vmtree.h"

int vmtree_height(const struct vm_node *n) { return n ? n->height : 0; }

static void fix(struct vm_node *n) {
    int l = vmtree_height(n->left), r = vmtree_height(n->right);
    n->height = (l > r ? l : r) + 1;
}

static struct vm_node *rot_right(struct vm_node *n) {
    struct vm_node *l = n->left;
    n->left  = l->right;
    l->right = n;
    fix(n);
    fix(l);
    return l;
}

static struct vm_node *rot_left(struct vm_node *n) {
    struct vm_node *r = n->right;
    n->right = r->left;
    r->left  = n;
    fix(n);
    fix(r);
    return r;
}

/* `n` with its children each balanced and at most two apart: balanced. */
static struct vm_node *balance(struct vm_node *n) {
    fix(n);
    int d = vmtree_height(n->left) - vmtree_height(n->right);
    if (d > 1) {
        if (vmtree_height(n->left->left) < vmtree_height(n->left->right))
            n->left = rot_left(n->left);
        return rot_right(n);
    }
    if (d < -1) {
        if (vmtree_height(n->right->right) < vmtree_height(n->right->left))
            n->right = rot_right(n->right);
        return rot_left(n);
    }
    return n;
}

struct vm_node *vmtree_find(struct vm_node *n, uint64_t addr) {
    while (n) {
        if (addr < n->base)                 n = n->left;
        else if (addr - n->base >= n->len)  n = n->right;
        else                                return n;
    }
    return 0;
}

/* The nodes do not overlap and are ordered by base, so a range that ends at
   or before a node's start can only meet nodes to its left, and one that
   starts at or after its end only nodes to its right. */
struct vm_node *vmtree_overlap(struct vm_node *n, uint64_t base, uint64_t len) {
    while (n) {
        if (base + len <= n->base)          n = n->left;
        else if (base >= n->base + n->len)  n = n->right;
        else                                return n;
    }
    return 0;
}

static struct vm_node *insert(struct vm_node *t, struct vm_node *n) {
    if (!t) return n;
    if (n->base < t->base) t->left  = insert(t->left, n);
    else                   t->right = insert(t->right, n);
    return balance(t);
}

int vmtree_insert(struct vm_node **root, struct vm_node *n) {
    if (vmtree_overlap(*root, n->base, n->len)) return -1;
    n->left = n->right = 0;
    n->height = 1;
    *root = insert(*root, n);
    return 0;
}

/* Take the lowest node out of `t`, into *min. */
static struct vm_node *remove_min(struct vm_node *t, struct vm_node **min) {
    if (!t->left) { *min = t; return t->right; }
    t->left = remove_min(t->left, min);
    return balance(t);
}

static struct vm_node *remove(struct vm_node *t, struct vm_node *n) {
    if (!t) return 0;
    if (n->base < t->base) { t->left  = remove(t->left, n);  return balance(t); }
    if (n->base > t->base) { t->right = remove(t->right, n); return balance(t); }
    /* t is n: bases are unique, since nodes do not overlap */
    if (!t->left)  return t->right;
    if (!t->right) return t->left;
    struct vm_node *s;
    struct vm_node *rest = remove_min(t->right, &s);
    s->left  = t->left;
    s->right = rest;
    return balance(s);
}

void vmtree_remove(struct vm_node **root, struct vm_node *n) {
    *root = remove(*root, n);
    n->left = n->right = 0;
}

uint64_t vmtree_first_fit(struct vm_node *n, uint64_t lo, uint64_t hi, uint64_t len) {
    /* In order, with a stack as deep as the tree can be (64 covers any tree
       that fits in memory); `at` moves past each node in the way. */
    struct vm_node *stack[64];
    int sp = 0;
    uint64_t at = lo;
    for (;;) {
        while (n) {
            if (sp == 64) return 0;
            stack[sp++] = n;
            n = n->left;
        }
        if (!sp) break;
        n = stack[--sp];
        if (n->base >= at + len && at + len >= at) break;   /* it fits before n */
        if (n->base + n->len > at) at = n->base + n->len;
        if (at >= hi) return 0;
        n = n->right;
    }
    if (at + len < at || at + len > hi) return 0;
    return at;
}

uint32_t vmtree_count(const struct vm_node *n) {
    return n ? 1u + vmtree_count(n->left) + vmtree_count(n->right) : 0u;
}
