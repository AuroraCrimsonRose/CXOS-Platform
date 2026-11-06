// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/physmap.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Reaching physical memory (1.9c). Code outside the architecture never adds
 * a base to a physical address: it maps the frame here, uses it, and unmaps
 * it. Only memman/physmap.c and the architecture know where a frame lands.
 *
 * A mapping is this CPU's and short. Interrupts are off from phys_map to
 * phys_unmap (the flags are kept with the mapping), so the thread is neither
 * switched nor moved, and the code between may not block. Mappings nest,
 * last in first out, PHYS_MAP_DEPTH deep: a page-table walk maps each level
 * under the one above. On x86-64 a frame's place is the direct map, and
 * nothing is mapped or flushed; the rules are kept and checked anyway, so
 * a port without a direct map (x86-32: a few pages per CPU, as Serenity's
 * MemoryManager::quickmap_page and Haiku's per-CPU slot queues in
 * x86_physical_page_mapper_large_memory.cpp) changes the mapping and
 * nothing else.
 *
 * Checked in every build, each a panic: an unmap that is not of the most
 * recent mapping, an unmap with none open, more than PHYS_MAP_DEPTH open, a
 * frame past RAM_REACH (XNU's PHYSMAP_PTOV_check, osfmk/i386/pmap.h), and a
 * thread switch with one open (cpu/sched.c). CX_PHYS_MEMORY.md has the rest.
 */
#ifndef CXK_PHYSMAP_H
#define CXK_PHYSMAP_H

#include <stdint.h>
#include "arch/paging.h"   /* paddr_t, PAGE_SIZE, RAM_REACH */

#define PHYS_MAP_DEPTH 8   /* mappings one CPU may hold at once */

/* What one CPU has mapped, oldest first (cpu/percpu.h holds one). */
struct physmap_open {
    uint32_t n;
    struct { paddr_t frame; uintptr_t at; uint32_t flags; } e[PHYS_MAP_DEPTH];
};

/* The kernel's view of `phys`, its offset within the page kept; a use may
   not run past the page's end. Interrupts are off until phys_unmap. */
void *phys_map(paddr_t phys);

/* End the most recent mapping on this CPU; `at` is any address inside its
   page (what phys_map returned will do). Interrupts as they were before it. */
void phys_unmap(const volatile void *at);

/* Zero one whole frame: every frame that reaches ring 3 is zeroed or
   wholly written first, since one off the free list holds whatever its
   last owner left. */
void phys_zero(paddr_t frame);

/* RAM [phys, phys+len) mapped for the kernel for good: the page database.
   Panics if the kernel cannot reach it. */
void *phys_map_lasting(paddr_t phys, uint64_t len);

/* How many mappings this CPU holds; at a thread switch, none (else a
   panic naming `where`). */
uint32_t phys_map_depth(void);
void     phys_assert_none(const char *where);

enum {
    PM_OK = 0,
    PM_DEPTH,         /* more than PHYS_MAP_DEPTH open */
    PM_REACH,         /* the frame is past RAM_REACH */
    PM_NOT_LAST,      /* unmapped, but not the most recent */
    PM_NONE_OPEN,     /* unmapped with nothing mapped */
    PM_SWITCH,        /* a thread switch with a mapping open */
};

#if !defined(CXK_HOST_TEST)
#include "config.h"
#if CXK_TEST_HOOKS
/* For the self-tests: while armed, a failure is counted, not a panic. A
   refused map returns 0 and keeps nothing; a refused unmap ends nothing; a
   switch goes ahead with the mapping still open. Disarm returns the first
   failure's PM_ code and, in *count, how many. */
void     physmap_test_arm(void);
uint32_t physmap_test_disarm(uint32_t *count);
#endif
#endif

#endif
