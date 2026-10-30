// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/vmtree.h */
/* Aurora Tejeda / CATX Systems */
/*
 * A process's regions in a balanced tree (docs/kernel/CX_KERNEL_PLAN.md
 * section 9.4): an AVL tree of non-overlapping address ranges, ordered by
 * start, as Haiku's VMUserAddressSpace, NT's VAD tree and Serenity's
 * RegionTree keep theirs. The fixed array it replaces had to be scanned
 * whole for every fault, map and unmap; here a lookup is O(log n).
 *
 * Intrusive and allocation-free: the caller owns each node (it is the first
 * member of memman/vmregion.h's struct vm_region) and the locking. Nothing
 * here touches the kernel, so the host tests run it as it is.
 */
#ifndef MEMMAN_VMTREE_H
#define MEMMAN_VMTREE_H

#include <stdint.h>

struct vm_node {
    uint64_t        base;     /* first byte */
    uint64_t        len;      /* bytes; never 0 */
    struct vm_node *left, *right;
    int             height;   /* a leaf is 1 */
};

/* The node holding `addr`, or 0. */
struct vm_node *vmtree_find(struct vm_node *root, uint64_t addr);

/* A node sharing a byte with [base, base+len), or 0. `len` is not 0 and
   base+len does not wrap. */
struct vm_node *vmtree_overlap(struct vm_node *root, uint64_t base, uint64_t len);

/* Add `n` (base and len set). 0, or -1 if it overlaps a node already there,
   and then the tree is unchanged. */
int vmtree_insert(struct vm_node **root, struct vm_node *n);

/* Take out `n`, which is in the tree. */
void vmtree_remove(struct vm_node **root, struct vm_node *n);

/* The lowest address in [lo, hi) where `len` bytes fit between the nodes, or
   0 if none does. lo is not 0. First fit, in address order. */
uint64_t vmtree_first_fit(struct vm_node *root, uint64_t lo, uint64_t hi, uint64_t len);

/* Nodes in the tree, and its height: for the tests. */
uint32_t vmtree_count(const struct vm_node *root);
int      vmtree_height(const struct vm_node *root);

#endif
