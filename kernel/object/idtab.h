// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/idtab.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Id tables: small integers naming kernel records, growing on demand
 * (docs/planning/CX_ROADMAP.md step 2: "thread and handle tables become
 * dynamic").
 *
 * Thread ids and pids were indexes into fixed arrays of 64, and every table
 * kept beside a thread was another array of 64 indexed the same way. An id
 * table maps an id to a pointer and nothing else; the record itself comes
 * from an object cache and holds what used to sit in the side arrays.
 *
 * An id is the LOWEST free one at or above the floor the caller asks for, so
 * ids stay small and dense the way the old slots were. The table starts empty
 * and doubles when full, up to a ceiling set at init; the old array is freed
 * once the new one is in place. Growth is the only allocation, and a table
 * that cannot grow refuses the id rather than failing later.
 *
 * Not locked. The scheduler's tables are read from the timer interrupt, so
 * their owner calls in with interrupts off (one CPU today); with SMP the owner
 * takes its lock around the same calls.
 */
#ifndef CXK_IDTAB_H
#define CXK_IDTAB_H

#include <stdint.h>

/* The first array an empty table allocates. */
#define IDTAB_FIRST_CAP 16

struct idtab {
    void   **slot;     /* cap entries; NULL = free */
    uint32_t cap;      /* entries allocated */
    uint32_t used;     /* entries non-NULL */
    uint32_t max;      /* ids are below this, always */
    uint32_t grows;    /* arrays allocated, for the tests */
};

/* An empty table whose ids will all be below `max`. Allocates nothing. */
void idtab_init(struct idtab *t, uint32_t max);

/* Give `obj` (not NULL) the lowest free id >= `floor`, growing the table if
   needed. Returns the id, or -1 if every id below the ceiling is taken or the
   table could not grow. */
int idtab_alloc(struct idtab *t, void *obj, uint32_t floor);

/* The record with this id, or NULL (free, or out of range: any int is safe). */
void *idtab_get(const struct idtab *t, int id);

/* Free the id; returns what it named (NULL if nothing). */
void *idtab_take(struct idtab *t, int id);

/* Free the array. The ids must all have been taken. */
void idtab_destroy(struct idtab *t);

/* Where the array comes from: kmalloc in the kernel, the test's own in the
   host tests (which can make it fail). */
void *idtab_mem_alloc(uint32_t bytes);
void  idtab_mem_free(void *p);

#endif
