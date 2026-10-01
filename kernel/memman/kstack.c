/* /kernel/memman/kstack.c */
/* Aurora Tejeda / CATX Systems LLC */
/* Guarded kernel stacks - see kstack.h. */

#include "kstack.h"
#include "paging.h"
#include "pmm.h"

struct kstack_slot {
    uint32_t base;    /* lowest mapped address; 0 = slot free */
    int      owner;   /* thread id, for the double-fault report */
};

static struct kstack_slot slots[KSTACK_SLOTS];
static uint32_t live = 0;

static uint32_t slot_start(uint32_t i) { return KSTACK_REGION_BASE + i * KSTACK_SLOT_SIZE; }

void kstack_init(void) {
    /* Force each 4 MB page table of the region into existence while the kernel
       space is the only one, the same way paging_init reserves the temp-map
       PDE. Creating one later would have to be propagated into every live
       space; created now, it is simply copied into each space as it is made. */
    for (uint32_t v = KSTACK_REGION_BASE; v < KSTACK_REGION_BASE + KSTACK_REGION_SIZE;
         v += 0x00400000u) {
        paging_map(v, 0, PAGE_PRESENT | PAGE_WRITE);
        paging_unmap(v);
    }
    for (uint32_t i = 0; i < KSTACK_SLOTS; i++) { slots[i].base = 0; slots[i].owner = -1; }
    live = 0;
}

uint32_t kstack_alloc(uint32_t bytes, int owner, uint32_t *base_out) {
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0 || pages >= KSTACK_SLOT_SIZE / PAGE_SIZE) return 0;   /* keep a guard */

    uint32_t i = 0;
    while (i < KSTACK_SLOTS && slots[i].base) i++;
    if (i == KSTACK_SLOTS) return 0;

    uint32_t top  = slot_start(i) + KSTACK_SLOT_SIZE;
    uint32_t base = top - pages * PAGE_SIZE;

    for (uint32_t p = 0; p < pages; p++) {
        void *frame = pmm_alloc();
        if (!frame) {
            /* undo the pages already mapped, so a failed allocation leaks nothing */
            for (uint32_t q = 0; q < p; q++) {
                uint32_t v = base + q * PAGE_SIZE;
                pmm_free((void *)(paging_get_phys(v) & ~0xFFFu));
                paging_unmap(v);
            }
            return 0;
        }
        paging_map(base + p * PAGE_SIZE, (uint32_t)frame, PAGE_PRESENT | PAGE_WRITE);
    }

    slots[i].base  = base;
    slots[i].owner = owner;
    live++;
    if (base_out) *base_out = base;
    return top;
}

void kstack_free(uint32_t base) {
    if (base < KSTACK_REGION_BASE || base >= KSTACK_REGION_BASE + KSTACK_REGION_SIZE) return;
    uint32_t i = (base - KSTACK_REGION_BASE) / KSTACK_SLOT_SIZE;
    if (slots[i].base != base) return;   /* not a stack we handed out */

    for (uint32_t v = base; v < slot_start(i) + KSTACK_SLOT_SIZE; v += PAGE_SIZE) {
        uint32_t phys = paging_get_phys(v);
        if (phys) pmm_free((void *)(phys & ~0xFFFu));
        paging_unmap(v);
    }
    slots[i].base  = 0;
    slots[i].owner = -1;
    live--;
}

int kstack_guard_owner(uint32_t addr) {
    if (addr < KSTACK_REGION_BASE || addr >= KSTACK_REGION_BASE + KSTACK_REGION_SIZE) return -1;
    uint32_t i = (addr - KSTACK_REGION_BASE) / KSTACK_SLOT_SIZE;
    if (!slots[i].base || addr >= slots[i].base) return -1;
    return slots[i].owner;
}

uint32_t kstack_live(void) { return live; }
