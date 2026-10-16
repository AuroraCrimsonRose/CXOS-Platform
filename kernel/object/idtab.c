// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/idtab.c */
/* Aurora Tejeda / CATX Systems */
/* Id tables: see idtab.h. */
#include "object/idtab.h"

void idtab_init(struct idtab *t, uint32_t max) {
    t->slot  = 0;
    t->cap   = 0;
    t->used  = 0;
    t->max   = max;
    t->grows = 0;
}

/* Double the array (or make the first), never past the ceiling. */
static int grow(struct idtab *t) {
    uint32_t cap = t->cap ? t->cap * 2 : IDTAB_FIRST_CAP;
    if (cap > t->max) cap = t->max;
    if (cap <= t->cap) return -1;
    void **s = (void **)idtab_mem_alloc(cap * (uint32_t)sizeof(void *));
    if (!s) return -1;
    for (uint32_t i = 0; i < cap; i++) s[i] = i < t->cap ? t->slot[i] : 0;
    void **old = t->slot;
    t->slot = s;          /* complete before it is published */
    t->cap  = cap;
    t->grows++;
    if (old) idtab_mem_free(old);
    return 0;
}

int idtab_alloc(struct idtab *t, void *obj, uint32_t floor) {
    if (!obj || floor >= t->max) return -1;
    for (;;) {
        for (uint32_t i = floor; i < t->cap; i++)
            if (!t->slot[i]) {
                t->slot[i] = obj;
                t->used++;
                return (int)i;
            }
        /* Grown until the floor is inside the array and a slot is free. */
        if (grow(t) != 0) return -1;
    }
}

void *idtab_get(const struct idtab *t, int id) {
    if (id < 0 || (uint32_t)id >= t->cap) return 0;
    return t->slot[id];
}

void *idtab_take(struct idtab *t, int id) {
    if (id < 0 || (uint32_t)id >= t->cap) return 0;
    void *o = t->slot[id];
    if (o) { t->slot[id] = 0; t->used--; }
    return o;
}

void idtab_destroy(struct idtab *t) {
    if (t->slot) idtab_mem_free(t->slot);
    t->slot = 0;
    t->cap  = 0;
    t->used = 0;
}

#ifndef CXK_HOST_TEST
#include "heap.h"
void *idtab_mem_alloc(uint32_t bytes) { return kmalloc(bytes); }
void  idtab_mem_free(void *p)         { kfree(p); }
#endif
