// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/memman/vmregion.c */
/* Aurora Tejeda / CATX Systems */

#include "vmregion.h"
#include "arch/paging.h"
#include "memman/ocache.h"
#include "object/kmem.h"
#include "sched.h"
#include "job.h"
#include "panic.h"
#include "logging.h"
#include "diag/testhook.h"

static struct ocache region_cache;

void vm_sys_init(void) {
    if (ocache_init(&region_cache, "vmregion", sizeof(struct vm_region)) != 0)
        kpanic("vm_sys_init: no region cache");
}

/* A process's records live in its process (cpu/proc.h); the pid names it. */
static struct vm_proc *vp(int pid) {
    struct proc *p = proc_by_pid(pid);
    return p ? &p->vm : 0;
}

/* The job its image and stack pages are charged to as well (cpu/job.h): every
   change to `charged` below changes the job's account by the same amount. A
   mapping's pages are charged to the job by their memory object instead. */
static struct job *jb(int pid) {
    return proc_by_pid(pid)->job;   /* vp(pid) was checked first */
}

/* The space pid's regions are mapped in: its own, or the kernel's for a
   process that has none (the self-tests' scratch processes). */
static paddr_t space_of(int pid) {
    struct proc *p = proc_by_pid(pid);
    return p->pd_phys ? p->pd_phys : addr_space_kernel();
}

static uint32_t page_up(uint32_t n) { return (n + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u); }

static uint32_t default_quota = VM_DEFAULT_QUOTA;
uint32_t vm_default_quota(void) { return default_quota; }
void     vm_set_default_quota(uint32_t bytes) { if (bytes) default_quota = bytes; }

/* Is [addr, addr+len) inside the mmap window and clear of every region?
   `addr` is a whole user address (ABI 12): it is compared at full width, so a
   hint past 4 GB is refused rather than cut down to one inside the window. */
static int range_free(struct vm_proc *p, uint64_t addr, uint32_t len) {
    if (addr < VM_MMAP_BASE) return 0;
    if (addr + len < addr)   return 0;              /* wrapped */
    if (addr + len > VM_MMAP_TOP) return 0;
    return vmtree_overlap(p->root, addr, len) == 0;
}

/* Mapping flags for a requested protection; paging_map_user makes them ring
   3. Every page of a region is its object's, so VM_BORROWED. */
static uint32_t vm_flags(uint32_t prot) {
    uint32_t f = VM_BORROWED;
    if (prot & MPROT_WRITE) f |= VM_WRITE;
    if (prot & MPROT_EXEC)  f |= VM_EXEC;
    return f;
}

/* Unmap a region's pages from `space`. Only pages its object holds can have
   been mapped, so the others are passed over. */
static void unmap_region(paddr_t space, struct vm_region *r) {
    uint32_t first = (uint32_t)(r->off / PAGE_SIZE);
    for (uint64_t at = r->guard; at < r->node.len; at += PAGE_SIZE)
        if (kmem_page_if_resident(r->mem, first + (uint32_t)((at - r->guard) / PAGE_SIZE)))
            paging_unmap_in(space, (uintptr_t)(r->node.base + at));
}

/* A region out of its tree and its pages unmapped: what it held on its
   object goes, the writer it counted first. */
static void region_free(struct vm_region *r) {
    if (r->prot & MPROT_WRITE) kmem_writer_drop(r->mem);
    kobj_unref(&r->mem->hdr);
    ocache_free(&region_cache, r);
}

void vm_proc_init(int pid, uint32_t quota, int parent) {
    if (!vp(pid)) return;
    struct vm_proc *p = vp(pid);
    if (p->live) vm_proc_reset(pid);   /* an account started over */
    p->lock    = (spinlock_t)SPINLOCK_INIT;
    p->root    = 0;
    p->count   = 0;
    p->mapped  = 0;
    p->charged = 0;
    p->live    = 1;

    if (quota == 0 || quota > default_quota) quota = default_quota;
    if (vp(parent) && vp(parent)->live && quota > vp(parent)->quota)
        quota = vp(parent)->quota;
    p->quota = quota;
}

void vm_proc_reset(int pid) {
    if (!vp(pid)) return;
    struct vm_proc *p = vp(pid);
    if (p->live) {
        paddr_t space = space_of(pid);
        while (p->root) {
            struct vm_region *r = (struct vm_region *)p->root;
            uint32_t f = spin_lock(&p->lock);
            vmtree_remove(&p->root, &r->node);
            spin_unlock(&p->lock, f);
            unmap_region(space, r);
            region_free(r);
        }
        job_uncharge_mem(jb(pid), p->charged);
    }
    p->live    = 0;
    p->quota   = 0;
    p->mapped  = 0;
    p->charged = 0;
    p->count   = 0;
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
    int rc = job_charge_mem(jb(pid), len);
    if (rc != E_OK) return rc;

    p->mapped  += len;
    p->charged += len;
    return E_OK;
}

void vm_uncharge(int pid, uint32_t bytes) {
    if (!vp(pid) || !vp(pid)->live) return;
    struct vm_proc *p = vp(pid);
    uint32_t len = page_up(bytes);
    /* Clamped rather than allowed to go negative: an over-refund would hand a
       process a quota larger than it was given, which is worse than losing
       track of a page. */
    if (len > p->charged) len = p->charged;
    p->mapped  -= len;
    p->charged -= len;
    job_uncharge_mem(jb(pid), len);
}

int vm_map(int pid, struct vm_map_args *a) { return vm_map_object(pid, a, 0, 0); }

int vm_map_object(int pid, struct vm_map_args *a, struct kmem *m, uint32_t rights) {
    if (!vp(pid) || !vp(pid)->live) return E_INVALID_ARGS;
    struct vm_proc *p = vp(pid);

    /* With no object given, a->object must ask for a new one: a number here
       names no handle the kernel looked up. */
    if (!m && a->object != MOBJ_ANON)       return E_INVALID_ARGS;
    if (a->reserved)                        return E_INVALID_ARGS;
    if (a->length == 0)                     return E_INVALID_ARGS;
    if (a->length > MMAP_MAX_BYTES)         return E_OUT_OF_RANGE;
    if (a->prot & ~(MPROT_READ | MPROT_WRITE | MPROT_EXEC)) return E_INVALID_ARGS;
    if (a->flags & ~MMAP_HINT)              return E_INVALID_ARGS;
    /* Refused rather than quietly downgraded, so nothing is ever written that
       depends on it being allowed. */
    if ((a->prot & MPROT_WRITE) && (a->prot & MPROT_EXEC)) return E_INVALID_ARGS;
    /* A mapping nothing may do anything with is a mistake, not a request. */
    if (!(a->prot & (MPROT_READ | MPROT_WRITE | MPROT_EXEC))) return E_INVALID_ARGS;

    uint32_t len = page_up((uint32_t)a->length);                  /* at most MMAP_MAX_BYTES */
    if (len < a->length) return E_OUT_OF_RANGE;                   /* rounding wrapped */

    if (!m) {
        /* A new object has no interior to be at an offset into. */
        if (a->offset) return E_INVALID_ARGS;
    } else {
        /* A view inside the object, on a page boundary, that the handle
           allows. The end is compared at 64 bits: an offset near 2^64 does
           not wrap round to fit. */
        if (a->offset & (PAGE_SIZE - 1u))                         return E_INVALID_ARGS;
        uint64_t size = (uint64_t)m->npages * PAGE_SIZE;
        if (a->offset > size || len > size - a->offset)           return E_OUT_OF_RANGE;
        if (!(rights & HRIGHT_MAP))                               return E_ACCESS_DENIED;
        if ((a->prot & MPROT_WRITE) && !(rights & HRIGHT_MAP_WRITE)) return E_ACCESS_DENIED;
        if ((a->prot & MPROT_EXEC)  && !(rights & HRIGHT_MAP_EXEC))  return E_ACCESS_DENIED;
    }

    if (p->mapped + len < p->mapped) return E_OUT_OF_RANGE;
    if (p->mapped + len > p->quota)  return E_NO_MEMORY;
    if (p->count >= MMAP_MAX_REGIONS) return E_NO_RESOURCES;
    if (fault_hit(FAULT_VMMAP)) return E_NO_MEMORY;

    /* The object, and its commit, before the region: a refusal here has
       taken nothing (kmem_create gives back what it charged). */
    int rc;
    if (m) kobj_ref(&m->hdr);
    else if (!(m = kmem_create(len / PAGE_SIZE, jb(pid), &rc))) return rc;
    /* A writable view is counted on the object until it goes, so a seal
       against writing waits for none to be left (kmem.h). */
    int writer = (a->prot & MPROT_WRITE) != 0;
    if (writer && (rc = kmem_writer_add(m)) != E_OK) { kobj_unref(&m->hdr); return rc; }

    struct vm_region *r = ocache_alloc(&region_cache);
    if (!r) {
        if (writer) kmem_writer_drop(m);
        kobj_unref(&m->hdr);
        return E_NO_MEMORY;
    }
    r->prot = a->prot;
    r->mem  = m;
    r->off  = a->offset;
    r->node.len = len;

    /* The hint is tried and then abandoned, never honoured half way. An
       unaligned or occupied hint falls back to placement rather than being
       rounded into something the caller did not ask for - `addr` reports
       the truth either way. */
    uint32_t f = spin_lock(&p->lock);
    uint64_t base = 0;
    if ((a->flags & MMAP_HINT) && (a->addr & (PAGE_SIZE - 1u)) == 0 &&
        a->addr && range_free(p, a->addr, len))
        base = a->addr;
    if (!base) base = vmtree_first_fit(p->root, VM_MMAP_BASE, VM_MMAP_TOP, len);
    if (base) {
        r->node.base = base;
        if (vmtree_insert(&p->root, &r->node) != 0) kpanic("vm_map: placed on a region");
        p->count++;
        p->mapped += len;
    }
    spin_unlock(&p->lock, f);
    if (!base) {
        ocache_free(&region_cache, r);
        if (writer) kmem_writer_drop(m);
        kobj_unref(&m->hdr);
        return E_NO_RESOURCES;
    }

    a->addr    = base;
    a->granted = len;
    return E_OK;
}

int vm_unmap(int pid, uint64_t addr, uint64_t length) {
    if (!vp(pid) || !vp(pid)->live) return E_INVALID_ARGS;
    struct vm_proc *p = vp(pid);

    /* A mapping is released whole or not at all. Partial release would mean
       splitting a region into two, which needs a second record - so the call
       could fail for lack of memory while trying to give memory BACK, which
       is the wrong way round. A caller wanting halves can ask for two
       mappings. */
    uint32_t f = spin_lock(&p->lock);
    struct vm_region *r = (struct vm_region *)vmtree_find(p->root, addr);
    if (!r || r->node.base != addr) { spin_unlock(&p->lock, f); return E_NOT_FOUND; }
    if (r->stack)                   { spin_unlock(&p->lock, f); return E_ACCESS_DENIED; }
    /* compared whole: a length past 32 bits matches no mapping */
    if (length && (length > MMAP_MAX_BYTES ||
                   page_up((uint32_t)length) != r->node.len)) {
        spin_unlock(&p->lock, f);
        return E_INVALID_ARGS;
    }
    vmtree_remove(&p->root, &r->node);
    p->count--;
    p->mapped -= (uint32_t)r->node.len;
    spin_unlock(&p->lock, f);

    /* Out of the tree first, so no fault maps a page back in behind us. */
    unmap_region(space_of(pid), r);
    region_free(r);
    return E_OK;
}

int vm_map_stack(int pid, uint32_t len, uint64_t *base, uint64_t *top) {
    if (!vp(pid) || !vp(pid)->live) return E_INVALID_ARGS;
    struct vm_proc *p = vp(pid);
    if (len == 0 || (len & (PAGE_SIZE - 1u)) || len > MMAP_MAX_BYTES) return E_INVALID_ARGS;
    if (p->mapped + len < p->mapped || p->mapped + len > p->quota) return E_NO_MEMORY;
    if (p->count >= MMAP_MAX_REGIONS) return E_NO_RESOURCES;

    /* As vm_map_object's anonymous case: the object and its commit first,
       so a refusal has taken nothing. */
    int rc;
    struct kmem *m = kmem_create(len / PAGE_SIZE, jb(pid), &rc);
    if (!m) return rc;
    if ((rc = kmem_writer_add(m)) != E_OK) { kobj_unref(&m->hdr); return rc; }
    struct vm_region *r = ocache_alloc(&region_cache);
    if (!r) { kmem_writer_drop(m); kobj_unref(&m->hdr); return E_NO_MEMORY; }
    r->prot     = MPROT_READ | MPROT_WRITE;
    r->mem      = m;
    r->off      = 0;
    r->guard    = PAGE_SIZE;
    r->stack    = 1;
    r->node.len = (uint64_t)len + PAGE_SIZE;

    uint32_t f = spin_lock(&p->lock);
    uint64_t at = vmtree_first_fit(p->root, VM_MMAP_BASE, VM_MMAP_TOP, (uint32_t)r->node.len);
    if (at) {
        r->node.base = at;
        if (vmtree_insert(&p->root, &r->node) != 0) kpanic("vm_map_stack: placed on a region");
        p->count++;
        p->mapped += len;
    }
    spin_unlock(&p->lock, f);
    if (!at) { region_free(r); return E_NO_RESOURCES; }
    *base = at;
    *top  = at + r->node.len;
    return E_OK;
}

int vm_unmap_stack(int pid, uint64_t base) {
    if (!vp(pid) || !vp(pid)->live) return E_NOT_FOUND;
    struct vm_proc *p = vp(pid);
    uint32_t f = spin_lock(&p->lock);
    struct vm_region *r = (struct vm_region *)vmtree_find(p->root, base);
    if (!r || r->node.base != base || !r->stack) { spin_unlock(&p->lock, f); return E_NOT_FOUND; }
    vmtree_remove(&p->root, &r->node);
    p->count--;
    p->mapped -= (uint32_t)(r->node.len - r->guard);
    spin_unlock(&p->lock, f);
    unmap_region(space_of(pid), r);
    region_free(r);
    return E_OK;
}

int vm_futex_key(int pid, uint64_t addr, struct kmem **mem, uint64_t *off) {
    if (!vp(pid) || !vp(pid)->live) return 0;
    struct vm_proc *p = vp(pid);
    uint32_t f = spin_lock(&p->lock);
    struct vm_region *r = (struct vm_region *)vmtree_find(p->root, addr);
    /* A stack's guard is no byte of its object (vm_fault). */
    if (r && addr < r->node.base + r->guard) r = 0;
    if (r) {
        *mem = r->mem;
        *off = r->off + (addr - r->node.base - r->guard);
        kobj_ref(&r->mem->hdr);   /* taken under the lock: an unmap cannot drop it first */
    }
    spin_unlock(&p->lock, f);
    return r != 0;
}

int vm_info(int pid, struct vm_usage *u) {
    if (!vp(pid) || !vp(pid)->live) return E_INVALID_ARGS;
    u->quota  = vp(pid)->quota;
    u->mapped = vp(pid)->mapped;
    return E_OK;
}

int vm_fault(int pid, uint64_t addr, int write, int exec) {
    if (!vp(pid) || !vp(pid)->live || addr >= USER_VLIMIT) return 0;
    struct vm_proc *p = vp(pid);
    int ok = 0;

    /* Under the lock to the end, so the region cannot be unmapped between
       finding it and mapping its page. */
    uint32_t f = spin_lock(&p->lock);
    struct vm_region *r = (struct vm_region *)vmtree_find(p->root, addr);
    /* A stack's guard is in its region and is no page of it: a touch there
       is the thread's fault. */
    if (r && addr < r->node.base + r->guard) r = 0;
    if (r && !(write && !(r->prot & MPROT_WRITE)) && !(exec && !(r->prot & MPROT_EXEC))) {
        uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1u);
        uint32_t index = (uint32_t)((r->off + (page - r->node.base - r->guard)) / PAGE_SIZE);
        paddr_t phys = kmem_page(r->mem, index);
        ok = paging_map_user_in(space_of(pid), (uintptr_t)page, phys, vm_flags(r->prot)) == 0;
    }
    spin_unlock(&p->lock, f);
    return ok;
}
