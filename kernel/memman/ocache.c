// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/ocache.c */
/* Aurora Tejeda / CATX Systems */
/* Object caches: see ocache.h. */
#include "memman/ocache.h"

/* One record per span of the window, found from an object's address alone.
   This table is the "records away from the data" of ocache.h. */
struct slab_rec {
    struct ocache *owner;   /* NULL: the span is free */
    uint64_t       free;    /* bit i set: object i is free */
    uint32_t       used;    /* objects handed out */
    int32_t        prev;    /* links in the owner's partial or full list */
    int32_t        next;
};

static struct slab_rec recs[OCACHE_SLOTS];
static spinlock_t      slots_lock = SPINLOCK_INIT;
static uint32_t        slot_hint;

static uintptr_t slot_va(int32_t s) {
    return ocache_window() + (uintptr_t)s * OCACHE_SPAN;
}

static uint64_t all_free(uint32_t n) {
    return n >= 64 ? ~0ull : ((1ull << n) - 1);
}

static void fill(uint8_t *p, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) p[i] = v;
}

static void list_push(int32_t *head, int32_t s) {
    recs[s].prev = -1;
    recs[s].next = *head;
    if (*head >= 0) recs[*head].prev = s;
    *head = s;
}

static void list_del(int32_t *head, int32_t s) {
    if (recs[s].prev >= 0) recs[recs[s].prev].next = recs[s].next;
    else                   *head = recs[s].next;
    if (recs[s].next >= 0) recs[recs[s].next].prev = recs[s].prev;
    recs[s].prev = recs[s].next = -1;
}

/* Claim a free span for `c`, or -1. */
static int32_t slot_claim(struct ocache *c) {
    int32_t s = -1;
    uint32_t f = spin_lock(&slots_lock);
    for (uint32_t k = 0; k < OCACHE_SLOTS; k++) {
        uint32_t t = (slot_hint + k) % OCACHE_SLOTS;
        if (!recs[t].owner) { s = (int32_t)t; break; }
    }
    if (s >= 0) {
        recs[s].owner = c;
        slot_hint = ((uint32_t)s + 1) % OCACHE_SLOTS;
    }
    spin_unlock(&slots_lock, f);
    return s;
}

static void slot_release(int32_t s) {
    uint32_t f = spin_lock(&slots_lock);
    recs[s].owner = 0;
    recs[s].free  = 0;
    recs[s].used  = 0;
    spin_unlock(&slots_lock, f);
}

int ocache_init(struct ocache *c, const char *name, uint32_t size) {
    if (size == 0 || size > OCACHE_SPAN) return -1;
    uint32_t sz = (size + OCACHE_ALIGN - 1) & ~(OCACHE_ALIGN - 1);

    /* The fewest pages that waste no more than an eighth; failing that, the
       count that wastes the smallest share. */
    uint32_t best_p = 0, best_n = 0, best_w = 0;
    for (uint32_t p = 1; p <= OCACHE_SPAN / OCACHE_PAGE; p++) {
        uint32_t bytes = p * OCACHE_PAGE;
        uint32_t n = bytes / sz;
        if (n > OCACHE_MAX_PER_SLAB) n = OCACHE_MAX_PER_SLAB;
        if (n == 0) continue;
        uint32_t w = bytes - n * sz;
        if (w * 8 <= bytes) { best_p = p; best_n = n; break; }
        if (!best_p || (uint64_t)w * (best_p * OCACHE_PAGE) < (uint64_t)best_w * bytes) {
            best_p = p; best_n = n; best_w = w;
        }
    }

    c->name     = name;
    c->size     = sz;
    c->pages    = best_p;
    c->per_slab = best_n;
    c->lock     = (spinlock_t)SPINLOCK_INIT;
    c->partial  = -1;
    c->full     = -1;
    c->live     = 0;
    c->slabs    = 0;
    return 0;
}

/* A new slab for `c`, on its partial list. Called with c->lock held. */
static int32_t slab_new(struct ocache *c) {
    int32_t s = slot_claim(c);
    if (s < 0) return -1;
    if (ocache_map(slot_va(s), c->pages) != 0) { slot_release(s); return -1; }

    /* Every object starts poisoned, so the check at allocation needs no
       special case for an object never handed out. */
    fill((uint8_t *)slot_va(s), OCACHE_POISON, c->pages * OCACHE_PAGE);
    recs[s].free = all_free(c->per_slab);
    recs[s].used = 0;
    list_push(&c->partial, s);
    c->slabs++;
    return s;
}

void *ocache_alloc(struct ocache *c) {
    uint32_t f = spin_lock(&c->lock);
    int32_t s = c->partial;
    if (s < 0) s = slab_new(c);
    if (s < 0) { spin_unlock(&c->lock, f); return 0; }

    struct slab_rec *r = &recs[s];
    uint32_t i = (uint32_t)__builtin_ctzll(r->free);
    r->free &= ~(1ull << i);
    r->used++;
    c->live++;
    if (r->free == 0) { list_del(&c->partial, s); list_push(&c->full, s); }
    uint8_t *o = (uint8_t *)slot_va(s) + i * c->size;
    spin_unlock(&c->lock, f);

    /* The object is ours now, so the check and the clear need no lock. */
    for (uint32_t k = 0; k < c->size; k++)
        if (o[k] != OCACHE_POISON) { ocache_panic("ocache: object written after free"); break; }
    fill(o, 0, c->size);
    return o;
}

/* Where `p` is, if it is inside the window: its slot and its offset there. */
static int locate(const void *p, int32_t *slot, uint32_t *off) {
    uintptr_t a = (uintptr_t)p, w = ocache_window();
    if (a < w || a - w >= (uintptr_t)OCACHE_SLOTS * OCACHE_SPAN) return 0;
    *slot = (int32_t)((a - w) / OCACHE_SPAN);
    *off  = (uint32_t)((a - w) % OCACHE_SPAN);
    return 1;
}

void ocache_free(struct ocache *c, void *obj) {
    int32_t s; uint32_t off;
    if (!locate(obj, &s, &off)) { ocache_panic("ocache_free: not an object cache address"); return; }

    uint32_t f = spin_lock(&c->lock);
    struct slab_rec *r = &recs[s];
    const char *bad = 0;
    if (r->owner != c)
        bad = "ocache_free: object of another cache";
    else if (off % c->size || off / c->size >= c->per_slab)
        bad = "ocache_free: not the start of an object";
    else if (r->free & (1ull << (off / c->size)))
        bad = "ocache_free: object already free";
    if (bad) { spin_unlock(&c->lock, f); ocache_panic(bad); return; }

    uint32_t i = off / c->size;
    fill((uint8_t *)obj, OCACHE_POISON, c->size);
    int was_full = (r->free == 0);
    r->free |= 1ull << i;
    r->used--;
    c->live--;

    if (r->used == 0) {
        /* Empty: the pages go back. The span stays claimed until they have,
           so nothing can map over it meanwhile. */
        list_del(was_full ? &c->full : &c->partial, s);
        c->slabs--;
        spin_unlock(&c->lock, f);
        ocache_unmap(slot_va(s), c->pages);
        slot_release(s);
        return;
    }
    if (was_full) { list_del(&c->full, s); list_push(&c->partial, s); }
    spin_unlock(&c->lock, f);
}

uint32_t ocache_live(struct ocache *c) {
    uint32_t f = spin_lock(&c->lock);
    uint32_t n = c->live;
    spin_unlock(&c->lock, f);
    return n;
}

uint32_t ocache_slabs(struct ocache *c) {
    uint32_t f = spin_lock(&c->lock);
    uint32_t n = c->slabs;
    spin_unlock(&c->lock, f);
    return n;
}

int ocache_owns(struct ocache *c, const void *p) {
    int32_t s; uint32_t off;
    if (!locate(p, &s, &off)) return 0;
    uint32_t f = spin_lock(&c->lock);
    int yes = recs[s].owner == c && recs[s].used > 0
           && off % c->size == 0 && off / c->size < c->per_slab
           && !(recs[s].free & (1ull << (off / c->size)));
    spin_unlock(&c->lock, f);
    return yes;
}

int ocache_in_window(const void *p) {
    int32_t s; uint32_t off;
    return locate(p, &s, &off);
}

struct ocache *ocache_owner(const void *p) {
    int32_t s; uint32_t off;
    if (!locate(p, &s, &off)) return 0;
    uint32_t f = spin_lock(&slots_lock);
    struct ocache *c = recs[s].owner;
    spin_unlock(&slots_lock, f);
    return c;
}

static int walk_list(struct ocache *c, int32_t s, int (*fn)(void *, void *), void *ctx) {
    for (; s >= 0; s = recs[s].next)
        for (uint32_t i = 0; i < c->per_slab; i++)
            if (!(recs[s].free & (1ull << i))) {
                int rc = fn((uint8_t *)slot_va(s) + i * c->size, ctx);
                if (rc) return rc;
            }
    return 0;
}

int ocache_walk(struct ocache *c, int (*fn)(void *obj, void *ctx), void *ctx) {
    uint32_t f = spin_lock(&c->lock);
    int rc = walk_list(c, c->partial, fn, ctx);
    if (!rc) rc = walk_list(c, c->full, fn, ctx);
    spin_unlock(&c->lock, f);
    return rc;
}

#ifndef CXK_HOST_TEST
#include "arch/paging.h"
#include "pmm.h"
#include "panic.h"

uintptr_t ocache_window(void) { return OCACHE_WINDOW_BASE; }

void ocache_panic(const char *why) { kpanic(why); }

void ocache_sys_init(void) {
    /* Each 4 MB page table of the window, made now while the kernel's is the
       only address space, so it is copied into every space as it is made
       rather than propagated into live ones later. kstack_init does the same. */
    for (uintptr_t v = OCACHE_WINDOW_BASE; v < OCACHE_WINDOW_BASE + OCACHE_SLOTS * OCACHE_SPAN;
         v += 0x00400000u) {
        paging_map_kernel(v, 0, VM_WRITE);
        paging_unmap(v);
    }
}

void ocache_unmap(uintptr_t va, uint32_t pages) {
    for (uint32_t p = 0; p < pages; p++) {
        uint32_t v = (uint32_t)va + p * OCACHE_PAGE;
        paddr_t phys = paging_get_phys(v) & ~(paddr_t)0xFFF;
        paging_unmap(v);
        if (phys) pmm_free(phys);
    }
}

int ocache_map(uintptr_t va, uint32_t pages) {
    for (uint32_t p = 0; p < pages; p++) {
        paddr_t frame = pmm_alloc();
        if (!frame) { ocache_unmap(va, p); return -1; }
        paging_map_kernel((uintptr_t)va + p * OCACHE_PAGE, frame, VM_WRITE);
    }
    return 0;
}
#endif
