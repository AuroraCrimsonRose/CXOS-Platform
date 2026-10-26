// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/vmregion.c */
/* Aurora Tejeda / CATX Systems */

#include "vmregion.h"
#include "arch/paging.h"
#include "pmm.h"
#include "sched.h"
#include "logging.h"

/* A process's records live in its process (cpu/proc.h); the pid names it. */
static struct vm_proc *vp(int pid) {
    struct proc *p = proc_by_pid(pid);
    return p ? &p->vm : 0;
}

static uint32_t page_up(uint32_t n) { return (n + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u); }

static uint32_t default_quota = VM_DEFAULT_QUOTA;
uint32_t vm_default_quota(void) { return default_quota; }
void     vm_set_default_quota(uint32_t bytes) { if (bytes) default_quota = bytes; }

/* Do [a, a+alen) and [b, b+blen) share a byte? Written with the ends compared
   rather than the starts so a zero-length range never counts as overlapping. */
static int overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
    return a < b + blen && b < a + alen;
}

/* Is [addr, addr+len) inside the mmap window and clear of every live region? */
static int range_free(struct vm_proc *p, uint32_t addr, uint32_t len) {
    if (addr < VM_MMAP_BASE) return 0;
    if (addr + len < addr)   return 0;              /* wrapped */
    if (addr + len > VM_MMAP_TOP) return 0;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++)
        if (p->r[i].base && overlaps(addr, len, p->r[i].base, p->r[i].len))
            return 0;
    return 1;
}

/* Lowest free address in the window that fits `len`, or 0 if the window is too
   fragmented to hold it.

   First fit, restarted: whenever the candidate collides with a region, it moves
   to that region's end and the scan begins again. Each restart clears at least
   one region permanently, so the loop cannot run more times than there are
   regions - which is why a bound of MMAP_MAX_REGIONS + 1 is a proof rather
   than a guess. */
static uint32_t place(struct vm_proc *p, uint32_t len) {
    uint32_t at = VM_MMAP_BASE;
    for (int guard = 0; guard <= MMAP_MAX_REGIONS; guard++) {
        if (at + len < at || at + len > VM_MMAP_TOP) return 0;
        int moved = 0;
        for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
            if (!p->r[i].base) continue;
            if (overlaps(at, len, p->r[i].base, p->r[i].len)) {
                at = p->r[i].base + p->r[i].len;
                moved = 1;
                break;
            }
        }
        if (!moved) return at;
    }
    return 0;
}

/* Mapping flags for a requested protection; paging_map_user makes them ring
   3. MPROT_EXEC passes through as VM_EXEC, which the architecture enforces
   where its MMU can: a 32-bit non-PAE page table has no no-execute bit, so
   there every mapped page is executable whatever was asked for. The request
   is recorded in the region either way. */
static uint32_t vm_flags(uint32_t prot) {
    uint32_t f = 0;
    if (prot & MPROT_WRITE) f |= VM_WRITE;
    if (prot & MPROT_EXEC)  f |= VM_EXEC;
    return f;
}

void vm_proc_init(int pid, uint32_t quota, int parent) {
    if (!vp(pid)) return;
    struct vm_proc *p = vp(pid);
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) p->r[i].base = 0;
    p->mapped = 0;
    p->live   = 1;

    if (quota == 0 || quota > default_quota) quota = default_quota;
    if (vp(parent) && vp(parent)->live && quota > vp(parent)->quota)
        quota = vp(parent)->quota;
    p->quota = quota;
}

void vm_proc_reset(int pid) {
    if (!vp(pid)) return;
    /* Bookkeeping only. The frames are freed by addr_space_release(),
       which walks the whole user half; freeing them here as well would be a
       double free. See the header. */
    vp(pid)->live   = 0;
    vp(pid)->quota  = 0;
    vp(pid)->mapped = 0;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) vp(pid)->r[i].base = 0;
}

uint32_t vm_quota_of(int pid) {
    if (!vp(pid) || !vp(pid)->live) return 0;
    return vp(pid)->quota;
}

int vm_charge(int pid, uint32_t bytes) {
    if (!vp(pid) || !vp(pid)->live) return E_INVALID_ARGS;
    struct vm_proc *p = vp(pid);

    /* The same three checks vm_map makes, in the same order and for the same
       reasons: a rounded length that wrapped, a sum that wrapped, and the
       ceiling itself. Two accounts kept by different arithmetic would disagree
       on exactly the inputs chosen to make them disagree. */
    uint32_t len = page_up(bytes);
    if (len < bytes)                 return E_OUT_OF_RANGE;
    if (p->mapped + len < p->mapped) return E_OUT_OF_RANGE;
    if (p->mapped + len > p->quota)  return E_NO_MEMORY;

    p->mapped += len;
    return E_OK;
}

void vm_uncharge(int pid, uint32_t bytes) {
    if (!vp(pid) || !vp(pid)->live) return;
    struct vm_proc *p = vp(pid);
    uint32_t len = page_up(bytes);
    /* Clamped rather than allowed to go negative: an over-refund would hand a
       process a quota larger than it was given, which is worse than losing
       track of a page. */
    p->mapped = (len > p->mapped) ? 0 : p->mapped - len;
}

/* Unmap and free the first `done` pages of a mapping that failed part way in.
   A half-built mapping must leave nothing behind: the caller gets an error and
   must be able to treat it as though it had never asked. */
static void undo(uint32_t base, uint32_t done) {
    for (uint32_t i = 0; i < done; i += PAGE_SIZE) {
        paddr_t phys = paging_get_phys(base + i);
        paging_unmap(base + i);
        if (phys) pmm_free(phys);
    }
}

int vm_map(int pid, struct mem_op_args *a) {
    if (!vp(pid) || !vp(pid)->live) return E_INVALID_ARGS;
    struct vm_proc *p = vp(pid);

    if (a->object != MOBJ_ANON)             return E_INVALID_ARGS;
    /* Anonymous memory has no interior to be at an offset into. Rejecting a
       non-zero offset keeps the field honest until there is an object it can
       mean something for, rather than silently ignoring what was asked. */
    if (a->offset_lo || a->offset_hi)       return E_INVALID_ARGS;
    if (a->length == 0)                     return E_INVALID_ARGS;
    if (a->length > MMAP_MAX_BYTES)         return E_OUT_OF_RANGE;
    if (a->prot & ~(MPROT_READ | MPROT_WRITE | MPROT_EXEC)) return E_INVALID_ARGS;
    if (a->flags & ~MMAP_HINT)              return E_INVALID_ARGS;
    /* Refused rather than quietly downgraded, so nothing is ever written that
       depends on it being allowed. The ABI explains why this is discipline
       today and enforcement later. */
    if ((a->prot & MPROT_WRITE) && (a->prot & MPROT_EXEC)) return E_INVALID_ARGS;
    /* A mapping nothing may do anything with is a mistake, not a request. */
    if (!(a->prot & (MPROT_READ | MPROT_WRITE | MPROT_EXEC))) return E_INVALID_ARGS;

    uint32_t len = page_up(a->length);
    if (len < a->length) return E_OUT_OF_RANGE;                   /* rounding wrapped */
    if (p->mapped + len < p->mapped) return E_OUT_OF_RANGE;
    if (p->mapped + len > p->quota)  return E_NO_MEMORY;

    int slot = -1;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++)
        if (!p->r[i].base) { slot = i; break; }
    if (slot < 0) return E_NO_RESOURCES;

    /* The hint is tried and then abandoned, never honoured half way. An
       unaligned or occupied hint falls back to placement rather than being
       rounded into something the caller did not ask for - a window that moved
       is more useful than one that silently landed elsewhere and still claims
       to be where it was asked for, because `addr` reports the truth either
       way. */
    uint32_t base = 0;
    if ((a->flags & MMAP_HINT) && (a->addr & (PAGE_SIZE - 1u)) == 0 &&
        a->addr && range_free(p, a->addr, len))
        base = a->addr;
    if (!base) base = place(p, len);
    if (!base) return E_NO_RESOURCES;

    uint32_t flags = vm_flags(a->prot);
    for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
        paddr_t frame = pmm_alloc();
        if (!frame) { undo(base, off); return E_NO_MEMORY; }

        /* Mapped writable first so the zeroing below can happen through this
           very mapping, then set to the protection that was asked for. The
           zeroing is not tidiness: a frame just off the free list may hold
           another process's memory, and handing that to a caller would leak
           whatever it was. */
        /* Unreachable unless the range check above let a kernel address through,
           but it unwinds the same way the allocation failure does: returning
           without undo() would leave the pages already mapped in this loop
           stranded in the address space with nothing owning them. */
        if (paging_map_user(base + off, frame, VM_WRITE) != 0) {
            pmm_free(frame);
            undo(base, off);
            return E_INVALID_ARGS;
        }
        uint32_t *z = (uint32_t *)(uintptr_t)(base + off);
        for (uint32_t i = 0; i < PAGE_SIZE / 4u; i++) z[i] = 0;

        /* The re-protect is checked for the same reason the mapping above is,
           and the consequence of not checking it is worse: the page is
           writable at this point, so a dropped failure hands the caller write
           access to a region it asked to be read-only - and on x86-32 without
           PAE there is no execute bit, so a writable page is also executable,
           which is the W^X combination the loader refuses by name. It cannot
           fail in practice, because the page table the first call allocated is
           the one this call reuses; that is a fact about the callee, which is
           exactly the kind of reasoning a caller should not be doing. */
        if (flags != VM_WRITE &&
            paging_map_user(base + off, frame, flags) != 0) {
            undo(base, off + PAGE_SIZE);   /* this page is mapped: unwind it too */
            return E_INVALID_ARGS;
        }
    }

    p->r[slot].base   = base;
    p->r[slot].len    = len;
    p->r[slot].prot   = a->prot;
    p->r[slot].object = a->object;
    p->r[slot].off_lo = a->offset_lo;
    p->r[slot].off_hi = a->offset_hi;
    p->mapped += len;

    a->addr    = base;
    a->granted = len;
    return E_OK;
}

int vm_unmap(int pid, uint32_t addr, uint32_t length) {
    if (!vp(pid) || !vp(pid)->live) return E_INVALID_ARGS;
    struct vm_proc *p = vp(pid);

    /* A mapping is released whole or not at all. Partial release would mean
       splitting a region into two, which needs a free slot to put the second
       half in - so the call could fail for lack of a slot while trying to give
       memory BACK, which is the wrong way round. Whole-region unmap has no
       such failure, and a caller wanting halves can ask for two mappings. */
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (p->r[i].base != addr) continue;
        if (length && page_up(length) != p->r[i].len) return E_INVALID_ARGS;

        undo(p->r[i].base, p->r[i].len);
        p->mapped -= p->r[i].len;
        p->r[i].base = 0;
        return E_OK;
    }
    return E_NOT_FOUND;
}

int vm_info(int pid, struct mem_op_args *a) {
    if (!vp(pid) || !vp(pid)->live) return E_INVALID_ARGS;
    a->quota  = vp(pid)->quota;
    a->mapped = vp(pid)->mapped;
    return E_OK;
}
