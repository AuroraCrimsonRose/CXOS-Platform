// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/buddy.c */
/* Aurora Tejeda / CATX Systems */
/* The buddy allocator over the page database (buddy.h). */

#include "memman/buddy.h"

static unsigned zone_of(const struct buddy *b, uint32_t pfn) {
    return pfn < b->low_end ? BUDDY_ZONE_LOW : BUDDY_ZONE_HIGH;
}

static void link(struct buddy *b, uint32_t pfn, unsigned order) {
    uint32_t *head = &b->head[zone_of(b, pfn)][order];
    struct page *p = &b->db[pfn];
    p->order = (uint8_t)order;
    p->prev = BUDDY_NONE;
    p->next = *head;
    if (*head != BUDDY_NONE) b->db[*head].prev = pfn;
    *head = pfn;
}

static void unlink(struct buddy *b, uint32_t pfn, unsigned order) {
    struct page *p = &b->db[pfn];
    if (p->prev != BUDDY_NONE) b->db[p->prev].next = p->next;
    else                       b->head[zone_of(b, pfn)][order] = p->next;
    if (p->next != BUDDY_NONE) b->db[p->next].prev = p->prev;
}

/* [pfn, pfn + 2^order) is marked free and holds no head; make it a block and
   merge it upward while its buddy is a free block of the same order. Blocks
   are aligned to their size and the zone boundary to the largest, so a buddy
   is always in the same zone; the check stays as the rule it relies on. */
static void release_block(struct buddy *b, uint32_t pfn, unsigned order) {
    unsigned z = zone_of(b, pfn);
    b->free_pages[z] += 1u << order;
    while (order < BUDDY_MAX_ORDER) {
        uint32_t bud = pfn ^ (1u << order);
        if (bud >= b->npages || zone_of(b, bud) != z) break;
        struct page *q = &b->db[bud];
        if (q->state != PG_FREE || q->order != order) break;
        unlink(b, bud, order);
        b->db[pfn > bud ? pfn : bud].order = PG_TAIL;
        pfn = pfn < bud ? pfn : bud;
        order++;
    }
    link(b, pfn, order);
}

/* The largest block that starts at `pfn` and fits in `count` pages. */
static unsigned chunk_order(uint32_t pfn, uint32_t count) {
    unsigned k = 0;
    while (k < BUDDY_MAX_ORDER && !(pfn & (1u << k)) && (2u << k) <= count) k++;
    return k;
}

/* Hand [pfn, pfn+count) over as free blocks, as large as alignment allows. */
static void release_range(struct buddy *b, uint32_t pfn, uint32_t count) {
    while (count) {
        unsigned k = chunk_order(pfn, count);
        uint32_t n = 1u << k;
        for (uint32_t i = 0; i < n; i++) {
            b->db[pfn + i].state = PG_FREE;
            b->db[pfn + i].order = PG_TAIL;
            b->db[pfn + i].refs  = 0;
        }
        release_block(b, pfn, k);
        pfn += n;
        count -= n;
    }
}

int buddy_init(struct buddy *b, struct page *db, uint32_t npages, uint32_t low_end) {
    if (low_end & ((1u << BUDDY_MAX_ORDER) - 1)) return BUDDY_E_RANGE;
    b->db = db;
    b->npages = npages;
    b->low_end = low_end;
    b->managed = 0;
    for (unsigned z = 0; z < BUDDY_ZONES; z++) {
        b->free_pages[z] = 0;
        for (unsigned k = 0; k < BUDDY_ORDERS; k++) b->head[z][k] = BUDDY_NONE;
    }
    for (uint32_t i = 0; i < npages; i++) {
        db[i].next = db[i].prev = BUDDY_NONE;
        db[i].refs = 0;
        db[i].state = PG_RESERVED;
        db[i].order = PG_TAIL;
        db[i].pad[0] = db[i].pad[1] = 0;
    }
    return 0;
}

uint32_t buddy_add(struct buddy *b, uint32_t pfn, uint32_t count) {
    if (pfn >= b->npages) return 0;
    if (count > b->npages - pfn) count = b->npages - pfn;
    uint32_t end = pfn + count, added = 0;
    while (pfn < end) {
        if (b->db[pfn].state != PG_RESERVED) { pfn++; continue; }
        uint32_t run = pfn;
        while (run < end && b->db[run].state == PG_RESERVED) run++;
        release_range(b, pfn, run - pfn);
        added += run - pfn;
        pfn = run;
    }
    b->managed += added;
    return added;
}

uint32_t buddy_alloc(struct buddy *b, unsigned order, int low_only) {
    if (order > BUDDY_MAX_ORDER) return BUDDY_NONE;
    static const unsigned prefer[] = { BUDDY_ZONE_HIGH, BUDDY_ZONE_LOW };
    for (unsigned i = low_only ? 1u : 0u; i < 2; i++) {
        unsigned z = prefer[i];
        for (unsigned k = order; k <= BUDDY_MAX_ORDER; k++) {
            uint32_t pfn = b->head[z][k];
            if (pfn == BUDDY_NONE) continue;
            unlink(b, pfn, k);
            while (k > order) {           /* split: the upper halves go back */
                k--;
                link(b, pfn + (1u << k), k);
            }
            for (uint32_t j = 0; j < (1u << order); j++) {
                b->db[pfn + j].state = PG_USED;
                b->db[pfn + j].order = PG_TAIL;
                b->db[pfn + j].refs  = 1;
            }
            b->free_pages[z] -= 1u << order;
            return pfn;
        }
    }
    return BUDDY_NONE;
}

int buddy_free(struct buddy *b, uint32_t pfn, uint32_t count) {
    if (pfn >= b->npages || count > b->npages - pfn) return BUDDY_E_RANGE;
    for (uint32_t i = 0; i < count; i++)
        if (b->db[pfn + i].state != PG_USED) return BUDDY_E_NOT_USED;
    release_range(b, pfn, count);
    return 0;
}

uint32_t buddy_free_count(const struct buddy *b) {
    return b->free_pages[BUDDY_ZONE_LOW] + b->free_pages[BUDDY_ZONE_HIGH];
}

unsigned buddy_order_for(uint32_t count) {
    if (count == 0) return BUDDY_ORDERS;
    unsigned k = 0;
    while (k < BUDDY_ORDERS && (1u << k) < count) k++;
    return k;
}
