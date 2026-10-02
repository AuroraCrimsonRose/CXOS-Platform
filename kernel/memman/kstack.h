/* /kernel/memman/kstack.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Guarded kernel stacks.
 *
 * Every kernel stack lives in its own 64 KB slot of a reserved kernel-virtual
 * region. The stack is mapped at the TOP of the slot; everything below it in
 * the slot is left unmapped. So a stack that runs off its bottom touches an
 * unmapped page and faults, instead of silently overwriting whatever heap
 * object used to sit below it - which is what a kmalloc'd stack did.
 *
 * Why 56 KB of guard and not the usual single page: a function with a large
 * frame (a 4 KB staging buffer, say) moves esp down by more than a page in one
 * step, and its first write can land BELOW a one-page guard, in whatever is
 * mapped there. With the next stack 56 KB away, a single frame would have to be
 * that large to jump the gap. The cost is address space only - unmapped pages
 * take no memory - and the kernel half has room to spare.
 *
 * A fault on a guard runs out of stack while being delivered, so it arrives as
 * a double fault; idt.c handles that on a stack of its own (a task gate) and
 * names the thread by asking kstack_guard_owner().
 */
#ifndef KSTACK_H
#define KSTACK_H

#include <stdint.h>

#define KSTACK_REGION_BASE  0xCF000000u   /* 16 MB, directly below the heap */
#define KSTACK_REGION_SIZE  0x01000000u
#define KSTACK_SLOT_SIZE    0x00010000u   /* 64 KB: stack at the top, guard below */
#define KSTACK_SLOTS        (KSTACK_REGION_SIZE / KSTACK_SLOT_SIZE)   /* 256 */

/* Thread 0 (kmain, then the self-tests and the idle loop): the size the boot
   stack in kernel.asm always was, which kmain moves off early in boot. */
#define KSTACK_MAIN_SIZE    16384u

/* Reserve the region's page tables. Call once after paging_init and before any
   address space other than the kernel's exists, so the page directory entries
   are part of the shared kernel half from the start. */
void kstack_init(void);

/* Allocate a guarded stack of `bytes` (rounded up to whole pages, at most the
   slot size less one guard page) for thread `owner`. Returns the stack's TOP
   (one past its highest byte - the initial esp), or 0 if out of slots or
   memory. *base_out receives the lowest mapped address, which kstack_free
   takes back. */
uint32_t kstack_alloc(uint32_t bytes, int owner, uint32_t *base_out);

/* Unmap and free a stack from kstack_alloc. 0 is a safe no-op. */
void kstack_free(uint32_t base);

/* If `addr` lies in the guard part of a live stack's slot, the thread that
   owns it; otherwise -1. Lets the double-fault handler say "thread 3 ran out
   of stack" rather than print an address and leave the reader to work it out. */
int kstack_guard_owner(uint32_t addr);

/* live stacks (for the self-test's leak check) */
uint32_t kstack_live(void);

#endif
