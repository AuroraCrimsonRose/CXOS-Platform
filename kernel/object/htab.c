// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/htab.c */
/* Aurora Tejeda / CATX Systems */
/* Per-process handle tables. See htab.h. */

#include "object/htab.h"
#ifndef CXK_HOST_TEST
#include "cxk_abi.h"
_Static_assert(HTAB_E_PERM == E_PERM && HTAB_E_INVAL == E_INVAL &&
               HTAB_E_NOMEM == E_NOMEM && HTAB_E_BADF == E_BADF,
               "htab.h error values must be the ABI's");
_Static_assert(HTAB_RIGHT_DUPLICATE == HRIGHT_DUPLICATE, "htab.h's duplicate right must be the ABI's");
#endif

/* Grant ids are system-wide, so a grant chain can cross processes. */
static atomic_u32 next_grant = { 1 };

static uint32_t grant_new(void) {
    uint32_t g;
    do g = atomic_fetch_add(&next_grant, 1); while (g == 0);   /* 0 means "none" */
    return g;
}

static int32_t value_of(uint32_t slot, uint32_t gen) {
    return (int32_t)((gen << HTAB_SLOT_BITS) | slot);
}

/* The entry `h` names, or 0. Called with the lock held. */
static struct htab_entry *entry_of(struct htab *t, int32_t h) {
    if (h <= 0) return 0;
    uint32_t slot = (uint32_t)h & ((1u << HTAB_SLOT_BITS) - 1);
    uint32_t gen  = (uint32_t)h >> HTAB_SLOT_BITS;
    if (slot >= t->cap) return 0;
    struct htab_entry *e = &t->e[slot];
    if (!e->obj || e->gen != gen) return 0;
    return e;
}

static int rights_allowed(const struct kobj *o, uint32_t rights) {
    const struct kobj_ops *ops = kobj_type_ops((enum kobj_type)o->type);
    return ops && (rights & ~(ops->rights_valid | HTAB_RIGHTS_GENERIC)) == 0;
}

/* Put `o` in the first free slot. Called with the lock held. HTAB_E_NOMEM
   when every slot is taken: the caller may grow the table and try again. */
static int32_t place(struct htab *t, struct kobj *o, uint32_t rights, uint32_t parent) {
    for (uint32_t i = 0; i < t->cap; i++) {
        struct htab_entry *e = &t->e[i];
        if (e->obj) continue;
        kobj_handle_opened(o);
        e->obj    = o;
        e->rights = rights;
        e->grant  = grant_new();
        e->parent = parent;
        return value_of(i, e->gen);
    }
    return HTAB_E_NOMEM;
}

/* Double the array (or make the first), never past t->max. Called WITHOUT
   the lock, which is held only to copy and publish: the allocator is not
   called with it held. 0 when the table has more room than it had (here or
   by another caller meanwhile), HTAB_E_NOMEM when it cannot. */
static int grow(struct htab *t) {
    uint32_t f = spin_lock(&t->lock);
    uint32_t cap = t->cap;
    spin_unlock(&t->lock, f);

    uint32_t want = cap ? cap * 2 : HTAB_FIRST_SLOTS;
    if (want > t->max) want = t->max;
    if (want <= cap) return HTAB_E_NOMEM;
    struct htab_entry *n = (struct htab_entry *)htab_mem_alloc(want * (uint32_t)sizeof *n);
    if (!n) return HTAB_E_NOMEM;

    struct htab_entry *spare = n;
    f = spin_lock(&t->lock);
    if (t->cap == cap) {               /* nobody grew it first */
        for (uint32_t i = 0; i < want; i++) {
            if (i < cap) { n[i] = t->e[i]; continue; }
            n[i].obj    = 0;
            n[i].rights = 0;
            n[i].gen    = 1;
            n[i].grant  = 0;
            n[i].parent = 0;
        }
        spare  = t->e;                 /* the old array, freed below */
        t->e   = n;
        t->cap = want;
    }
    spin_unlock(&t->lock, f);
    if (spare) htab_mem_free(spare);
    return 0;
}

void htab_init(struct htab *t, uint32_t max) {
    t->lock.locked = 0;
    t->e   = 0;
    t->cap = 0;
    t->max = max > HTAB_MAX_SLOTS ? HTAB_MAX_SLOTS : max;
}

void htab_destroy(struct htab *t) {
    if (!t) return;
    uint32_t f = spin_lock(&t->lock);
    struct htab_entry *e = t->e;
    t->e   = 0;
    t->cap = 0;
    spin_unlock(&t->lock, f);
    if (e) htab_mem_free(e);
}

int32_t htab_install(struct htab *t, struct kobj *o, uint32_t rights, uint32_t parent_grant) {
    if (!t || !o) return HTAB_E_INVAL;
    if (!kobj_type_ops((enum kobj_type)o->type)) return HTAB_E_INVAL;
    if (!rights_allowed(o, rights)) return HTAB_E_PERM;

    for (;;) {
        uint32_t f = spin_lock(&t->lock);
        int32_t h = place(t, o, rights, parent_grant);
        spin_unlock(&t->lock, f);
        if (h != HTAB_E_NOMEM || grow(t) != 0) return h;
    }
}

struct kobj *htab_ref(struct htab *t, int32_t h, enum kobj_type type, uint32_t need,
                      struct htab_info *info) {
    if (!t) return 0;
    struct kobj *o = 0;
    uint32_t f = spin_lock(&t->lock);
    struct htab_entry *e = entry_of(t, h);
    if (e && (type == KOBJ_NONE || e->obj->type == type) && (e->rights & need) == need) {
        o = e->obj;
        kobj_ref(o);                     /* the handle's own reference keeps it alive until here */
        if (info) { info->rights = e->rights; info->grant = e->grant; info->parent = e->parent; }
    }
    spin_unlock(&t->lock, f);
    return o;
}

int32_t htab_dup(struct htab *t, int32_t h, uint32_t rights) {
    if (!t) return HTAB_E_BADF;
    for (;;) {
        /* Checked afresh after a growth: the source may have been closed. */
        uint32_t f = spin_lock(&t->lock);
        struct htab_entry *e = entry_of(t, h);
        int32_t r;
        if (!e)                            r = HTAB_E_BADF;
        else if (!(e->rights & HTAB_RIGHT_DUPLICATE)) r = HTAB_E_PERM;   /* not copyable */
        else if (rights & ~e->rights)      r = HTAB_E_PERM;   /* rights only shrink */
        else                               r = place(t, e->obj, rights, e->grant);
        spin_unlock(&t->lock, f);
        if (r != HTAB_E_NOMEM || grow(t) != 0) return r;
    }
}

int htab_close(struct htab *t, int32_t h) {
    if (!t) return HTAB_E_BADF;
    uint32_t f = spin_lock(&t->lock);
    struct htab_entry *e = entry_of(t, h);
    struct kobj *o = 0;
    if (e) {
        o = e->obj;
        e->obj    = 0;
        e->rights = 0;
        e->grant  = 0;
        e->parent = 0;
        e->gen    = (e->gen >= HTAB_GEN_MAX) ? 1 : e->gen + 1;
    }
    spin_unlock(&t->lock, f);

    if (!o) return HTAB_E_BADF;
    kobj_handle_closed(o);               /* hooks run outside the lock */
    return 0;
}

void htab_close_all(struct htab *t) {
    if (!t) return;
    for (uint32_t i = 0; ; i++) {
        uint32_t f = spin_lock(&t->lock);
        if (i >= t->cap) { spin_unlock(&t->lock, f); break; }
        int32_t h = t->e[i].obj ? value_of(i, t->e[i].gen) : 0;
        spin_unlock(&t->lock, f);
        if (h) htab_close(t, h);
    }
}

uint32_t htab_count(struct htab *t) {
    if (!t) return 0;
    uint32_t n = 0;
    uint32_t f = spin_lock(&t->lock);
    for (uint32_t i = 0; i < t->cap; i++) if (t->e[i].obj) n++;
    spin_unlock(&t->lock, f);
    return n;
}

#ifndef CXK_HOST_TEST
#include "heap.h"
void *htab_mem_alloc(uint32_t bytes) { return kmalloc(bytes); }
void  htab_mem_free(void *p)         { kfree(p); }
#endif
