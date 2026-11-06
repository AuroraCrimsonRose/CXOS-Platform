// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/kstack.c */
/* Aurora Tejeda / CATX Systems */
/* Guarded kernel stacks - see kstack.h. */

#include "kstack.h"
#include "arch/paging.h"
#include "pmm.h"
#include "diag/testhook.h"
#include "sync/spinlock.h"

struct kstack_slot {
    uintptr_t base;   /* lowest mapped address; 0 = slot free, SLOT_BUSY
                         while its pages are being mapped or freed */
    int      owner;   /* thread id, for the double-fault report */
};

/* Claimed or being given back: not free, and no stack to report. Below
   the region, so kstack_guard_owner's `addr >= base` leaves it alone. */
#define SLOT_BUSY ((uintptr_t)1)

/* The slots and the count, taken only to claim or give back a slot: the
   pages are mapped and freed outside it, so nothing nests under it. */
static spinlock_t slots_lock = SPINLOCK_INIT(LR_KSTACK);
static struct kstack_slot slots[KSTACK_SLOTS];
static uint32_t live = 0;

static uintptr_t slot_start(uint32_t i) { return KSTACK_REGION_BASE + i * KSTACK_SLOT_SIZE; }

void kstack_init(void) {
    /* Force each page table of the region into existence while the kernel
       space is the only one. Creating one later would have to be
       propagated into every live space; created now, it is simply copied into each space as it is made,
       and a stack's pages free after it goes are the count from before. */
    for (uintptr_t v = KSTACK_REGION_BASE; v < KSTACK_REGION_BASE + KSTACK_REGION_SIZE;
         v += PAGE_TABLE_SPAN) {
        paging_map_kernel(v, 0, VM_WRITE);
        paging_unmap(v);
    }
    for (uint32_t i = 0; i < KSTACK_SLOTS; i++) { slots[i].base = 0; slots[i].owner = -1; }
    live = 0;
}

uintptr_t kstack_alloc(uint32_t bytes, int owner, uintptr_t *base_out) {
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0 || pages >= KSTACK_SLOT_SIZE / PAGE_SIZE) return 0;   /* keep a guard */
    if (fault_hit(FAULT_KSTACK)) return 0;

    uint32_t f = spin_lock(&slots_lock);
    uint32_t i = 0;
    while (i < KSTACK_SLOTS && slots[i].base) i++;
    if (i < KSTACK_SLOTS) slots[i].base = SLOT_BUSY;
    spin_unlock(&slots_lock, f);
    if (i == KSTACK_SLOTS) return 0;

    uintptr_t top  = slot_start(i) + KSTACK_SLOT_SIZE;
    uintptr_t base = top - pages * PAGE_SIZE;

    for (uint32_t p = 0; p < pages; p++) {
        paddr_t frame = pmm_alloc();
        if (!frame) {
            /* undo the pages already mapped, so a failed allocation leaks nothing */
            for (uint32_t q = 0; q < p; q++) {
                uintptr_t v = base + q * PAGE_SIZE;
                pmm_free(paging_get_phys(v) & ~(paddr_t)0xFFF);
                paging_unmap(v);
            }
            f = spin_lock(&slots_lock);
            slots[i].base = 0;
            spin_unlock(&slots_lock, f);
            return 0;
        }
        paging_map_kernel(base + p * PAGE_SIZE, frame, VM_WRITE);
    }

    f = spin_lock(&slots_lock);
    slots[i].owner = owner;
    slots[i].base  = base;
    live++;
    spin_unlock(&slots_lock, f);
    if (base_out) *base_out = base;
    return top;
}

void kstack_free(uintptr_t base) {
    if (base < KSTACK_REGION_BASE || base >= KSTACK_REGION_BASE + KSTACK_REGION_SIZE) return;
    uint32_t i = (uint32_t)((base - KSTACK_REGION_BASE) / KSTACK_SLOT_SIZE);
    uint32_t f = spin_lock(&slots_lock);
    int ours = slots[i].base == base;    /* else not a stack we handed out */
    if (ours) { slots[i].base = SLOT_BUSY; slots[i].owner = -1; }
    spin_unlock(&slots_lock, f);
    if (!ours) return;

    for (uintptr_t v = base; v < slot_start(i) + KSTACK_SLOT_SIZE; v += PAGE_SIZE) {
        paddr_t phys = paging_get_phys(v);
        if (phys) pmm_free(phys & ~(paddr_t)0xFFF);
        paging_unmap(v);
    }
    f = spin_lock(&slots_lock);
    slots[i].base = 0;
    live--;
    spin_unlock(&slots_lock, f);
}

int kstack_guard_owner(uintptr_t addr) {
    if (addr < KSTACK_REGION_BASE || addr >= KSTACK_REGION_BASE + KSTACK_REGION_SIZE) return -1;
    uint32_t i = (uint32_t)((addr - KSTACK_REGION_BASE) / KSTACK_SLOT_SIZE);
    if (!slots[i].base || addr >= slots[i].base) return -1;
    return slots[i].owner;
}

uint32_t kstack_live(void) { return live; }
