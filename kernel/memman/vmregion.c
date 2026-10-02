/* /kernel/memman/vmregion.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */

#include "vmregion.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "logging.h"

struct vm_region {
    uint32_t base;       /* page-aligned start; 0 = this slot is free */
    uint32_t len;        /* page-aligned length */
    uint32_t prot;       /* MPROT_* as requested */
    uint32_t object;     /* MOBJ_* this is a view of */
    uint32_t off_lo;     /* 64-bit offset into that object */
    uint32_t off_hi;
};

struct vm_proc {
    int      live;
    uint32_t quota;      /* ceiling in bytes */
    uint32_t mapped;     /* how much of it is in use */
    struct vm_region r[MMAP_MAX_REGIONS];
};

static struct vm_proc procs[MAX_THREADS];

static uint32_t page_up(uint32_t n) { return (n + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u); }

static uint32_t default_quota = VM_DEFAULT_QUOTA;
uint32_t vm_default_quota(void) { return default_quota; }
void     vm_set_default_quota(uint32_t bytes) { if (bytes) default_quota = bytes; }

static int pid_ok(int pid) { return pid >= 0 && pid < MAX_THREADS; }

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

/* PTE flags for a requested protection. PAGE_USER always - this is ring-3
   memory by definition. PAGE_WRITE only when asked, and that one IS enforced.
   MPROT_EXEC has no bit to set: a 32-bit non-PAE PTE has no no-execute flag,
   so every mapped page is executable whatever was asked for. The request is
   still recorded in the region, so the day this kernel gains PAE or moves to
   64 bits, the missing enforcement is a change here and nowhere else. */
static uint32_t pte_flags(uint32_t prot) {
    uint32_t f = PAGE_PRESENT | PAGE_USER;
    if (prot & MPROT_WRITE) f |= PAGE_WRITE;
    return f;
}

void vm_proc_init(int pid, uint32_t quota, int parent) {
    if (!pid_ok(pid)) return;
    struct vm_proc *p = &procs[pid];
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) p->r[i].base = 0;
    p->mapped = 0;
    p->live   = 1;

    if (quota == 0 || quota > default_quota) quota = default_quota;
    if (pid_ok(parent) && procs[parent].live && quota > procs[parent].quota)
        quota = procs[parent].quota;
    p->quota = quota;
}

void vm_proc_reset(int pid) {
    if (!pid_ok(pid)) return;
    /* Bookkeeping only. The frames are freed by addr_space_reclaim_user(),
       which walks the whole user half; freeing them here as well would be a
       double free. See the header. */
    procs[pid].live   = 0;
    procs[pid].quota  = 0;
    procs[pid].mapped = 0;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) procs[pid].r[i].base = 0;
}

uint32_t vm_quota_of(int pid) {
    if (!pid_ok(pid) || !procs[pid].live) return 0;
    return procs[pid].quota;
}

int vm_charge(int pid, uint32_t bytes) {
    if (!pid_ok(pid) || !procs[pid].live) return E_INVAL;
    struct vm_proc *p = &procs[pid];

    /* The same three checks vm_map makes, in the same order and for the same
       reasons: a rounded length that wrapped, a sum that wrapped, and the
       ceiling itself. Two accounts kept by different arithmetic would disagree
       on exactly the inputs chosen to make them disagree. */
    uint32_t len = page_up(bytes);
    if (len < bytes)                 return E_RANGE;
    if (p->mapped + len < p->mapped) return E_RANGE;
    if (p->mapped + len > p->quota)  return E_NOMEM;

    p->mapped += len;
    return E_OK;
}

void vm_uncharge(int pid, uint32_t bytes) {
    if (!pid_ok(pid) || !procs[pid].live) return;
    struct vm_proc *p = &procs[pid];
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
        uint32_t phys = paging_get_phys(base + i);
        paging_unmap(base + i);
        if (phys) pmm_free((void *)phys);
    }
}

int vm_map(int pid, struct mem_op_args *a) {
    if (!pid_ok(pid) || !procs[pid].live) return E_INVAL;
    struct vm_proc *p = &procs[pid];

    if (a->object != MOBJ_ANON)             return E_INVAL;
    /* Anonymous memory has no interior to be at an offset into. Rejecting a
       non-zero offset keeps the field honest until there is an object it can
       mean something for, rather than silently ignoring what was asked. */
    if (a->offset_lo || a->offset_hi)       return E_INVAL;
    if (a->length == 0)                     return E_INVAL;
    if (a->length > MMAP_MAX_BYTES)         return E_RANGE;
    if (a->prot & ~(MPROT_READ | MPROT_WRITE | MPROT_EXEC)) return E_INVAL;
    if (a->flags & ~MMAP_HINT)              return E_INVAL;
    /* Refused rather than quietly downgraded, so nothing is ever written that
       depends on it being allowed. The ABI explains why this is discipline
       today and enforcement later. */
    if ((a->prot & MPROT_WRITE) && (a->prot & MPROT_EXEC)) return E_INVAL;
    /* A mapping nothing may do anything with is a mistake, not a request. */
    if (!(a->prot & (MPROT_READ | MPROT_WRITE | MPROT_EXEC))) return E_INVAL;

    uint32_t len = page_up(a->length);
    if (len < a->length) return E_RANGE;                   /* rounding wrapped */
    if (p->mapped + len < p->mapped) return E_RANGE;
    if (p->mapped + len > p->quota)  return E_NOMEM;

    int slot = -1;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++)
        if (!p->r[i].base) { slot = i; break; }
    if (slot < 0) return E_NOMEM;

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
    if (!base) return E_NOMEM;

    uint32_t flags = pte_flags(a->prot);
    for (uint32_t off = 0; off < len; off += PAGE_SIZE) {
        void *frame = pmm_alloc();
        if (!frame) { undo(base, off); return E_NOMEM; }

        /* Mapped writable first so the zeroing below can happen through this
           very mapping, then set to the protection that was asked for. The
           zeroing is not tidiness: a frame just off the free list may hold
           another process's memory, and handing that to a caller would leak
           whatever it was. */
        /* Unreachable unless the range check above let a kernel address through,
           but it unwinds the same way the allocation failure does: returning
           without undo() would leave the pages already mapped in this loop
           stranded in the address space with nothing owning them. */
        if (paging_map_user(base + off, (uint32_t)frame, PAGE_PRESENT | PAGE_WRITE) != 0) {
            pmm_free(frame);
            undo(base, off);
            return E_INVAL;
        }
        uint32_t *z = (uint32_t *)(base + off);
        for (uint32_t i = 0; i < PAGE_SIZE / 4u; i++) z[i] = 0;
        if (flags != (PAGE_PRESENT | PAGE_WRITE | PAGE_USER))
            paging_map_user(base + off, (uint32_t)frame, flags);
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
    if (!pid_ok(pid) || !procs[pid].live) return E_INVAL;
    struct vm_proc *p = &procs[pid];

    /* A mapping is released whole or not at all. Partial release would mean
       splitting a region into two, which needs a free slot to put the second
       half in - so the call could fail for lack of a slot while trying to give
       memory BACK, which is the wrong way round. Whole-region unmap has no
       such failure, and a caller wanting halves can ask for two mappings. */
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (p->r[i].base != addr) continue;
        if (length && page_up(length) != p->r[i].len) return E_INVAL;

        undo(p->r[i].base, p->r[i].len);
        p->mapped -= p->r[i].len;
        p->r[i].base = 0;
        return E_OK;
    }
    return E_NOENT;
}

int vm_info(int pid, struct mem_op_args *a) {
    if (!pid_ok(pid) || !procs[pid].live) return E_INVAL;
    a->quota  = procs[pid].quota;
    a->mapped = procs[pid].mapped;
    return E_OK;
}
