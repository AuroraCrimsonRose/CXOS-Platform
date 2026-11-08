// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kmem.c */
/* Aurora Tejeda / CATX Systems */
/* Memory objects: see kmem.h. */
#include "object/kmem.h"
#include "memman/ocache.h"
#include "memman/heap.h"
#include "pmm.h"
#include "memman/physmap.h"
#include "job.h"
#include "string.h"
#include "cxk_abi.h"
#include "panic.h"
#include "drivers/devhost.h"
#include "fb.h"

_Static_assert(KOBJ_MEM == OBJ_TYPE_MEM, "object_basic.kind is the kernel type");

static struct ocache mem_cache;

/* Its pages, what was reserved for the rest, and the job's charge. Nothing
   maps it any more: every mapping held a reference. */
static void mem_destroy(struct kobj *o) {
    struct kmem *m = (struct kmem *)o;
    if (m->kind == KMEM_DEVICE) {
        /* The registers are the device's: nothing to give back but the
           records, and the hold on the device. */
        struct kdevice *dev = m->dev;
        kfree(m->pages);
        ocache_free(&mem_cache, m);
        devhost_put(dev);
        return;
    }
    if (m->kind == KMEM_SCREEN) {
        /* Nothing maps it now (every mapping held a reference): the kernel
           may have the screen back. */
        kfree(m->pages);
        ocache_free(&mem_cache, m);
        fb_screen_release();
        return;
    }
    if (m->kind == KMEM_DMA) {
        /* The device may still write these pages: they wait in its list
           until it closes (devhost_dma_retire, then kmem_dma_release). */
        devhost_dma_retire(m->dev, m);
        return;
    }
    for (uint32_t i = 0; i < m->npages; i++)
        if (m->pages[i]) pmm_free(m->pages[i]);
    pmm_unreserve(m->npages - m->resident);
    job_uncharge_mem(m->job, (uint64_t)m->npages * PAGE_SIZE);
    job_put(m->job);
    kfree(m->pages);
    ocache_free(&mem_cache, m);
}

static const struct kobj_ops mem_ops = {
    .name         = "memory",
    .rights_valid = HRIGHT_INSPECT | HRIGHT_MAP | HRIGHT_MAP_WRITE | HRIGHT_MAP_EXEC |
                    HRIGHT_SEAL,
    /* a receiver that must trust the bytes checks for MEM_SEAL_WRITE */
    .transfer     = 1,
    .destroy      = mem_destroy,
};

static const struct kobj_ops lease_ops;

void kmem_sys_init(void) {
    if (ocache_init(&mem_cache, "memory", sizeof(struct kmem)) != 0 ||
        kobj_type_register(KOBJ_MEM, &mem_ops) != 0 ||
        kobj_type_register(KOBJ_LEASE, &lease_ops) != 0)
        kpanic("kmem_sys_init: no memory object cache");
}

struct kmem *kmem_create(uint32_t npages, struct job *job, int *rc) {
    *rc = E_NO_MEMORY;
    if (npages == 0 || npages > MMAP_MAX_BYTES / PAGE_SIZE) { *rc = E_OUT_OF_RANGE; return 0; }

    /* The job first, then the pages, then the records, each undone if a
       later one is refused. */
    int r = job_charge_mem(job, (uint64_t)npages * PAGE_SIZE);
    if (r != E_OK) { *rc = r; return 0; }
    if (pmm_reserve(npages) != 0) goto no_pages;

    struct kmem *m = ocache_alloc(&mem_cache);
    if (!m) goto no_object;
    m->pages = kmalloc((size_t)npages * sizeof(paddr_t));
    if (!m->pages || kobj_init(&m->hdr, KOBJ_MEM) != 0) {
        kfree(m->pages);
        ocache_free(&mem_cache, m);
        goto no_object;
    }
    memset(m->pages, 0, (size_t)npages * sizeof(paddr_t));
    spin_init(&m->lock, LR_KMEM);
    m->npages   = npages;
    m->resident = 0;
    m->writers  = 0;
    m->seals    = 0;
    kobj_ref(&job->hdr);
    m->job      = job;
    m->kind     = KMEM_ANON;
    m->phys     = 0;
    m->dev      = 0;
    m->qnext    = 0;
    *rc = E_OK;
    return m;

no_object:
    pmm_unreserve(npages);
no_pages:
    job_uncharge_mem(job, (uint64_t)npages * PAGE_SIZE);
    return 0;
}

/* The records of an object whose pages are all present from the start: the
   array filled from `base`, nothing reserved, nothing yet charged. */
static struct kmem *fixed_create(paddr_t base, uint32_t npages, uint32_t kind,
                                 struct kdevice *dev) {
    struct kmem *m = ocache_alloc(&mem_cache);
    if (!m) return 0;
    m->pages = kmalloc((size_t)npages * sizeof(paddr_t));
    if (!m->pages || kobj_init(&m->hdr, KOBJ_MEM) != 0) {
        kfree(m->pages);
        ocache_free(&mem_cache, m);
        return 0;
    }
    for (uint32_t i = 0; i < npages; i++) m->pages[i] = base + (paddr_t)i * PAGE_SIZE;
    spin_init(&m->lock, LR_KMEM);
    m->npages   = npages;
    m->resident = npages;
    m->writers  = 0;
    m->seals    = 0;
    m->job      = 0;
    m->kind     = kind;
    m->phys     = base;
    if (dev) devhost_get(dev);   /* the screen's has none */
    m->dev      = dev;
    m->qnext    = 0;
    return m;
}

struct kmem *kmem_create_device(paddr_t base, uint32_t npages, struct kdevice *dev, int *rc) {
    if (npages == 0 || npages > MMAP_MAX_BYTES / PAGE_SIZE) { *rc = E_OUT_OF_RANGE; return 0; }
    struct kmem *m = fixed_create(base, npages, KMEM_DEVICE, dev);
    *rc = m ? E_OK : E_NO_MEMORY;
    return m;
}

struct kmem *kmem_create_screen(paddr_t base, uint32_t npages, int *rc) {
    if (npages == 0 || npages > MMAP_MAX_BYTES / PAGE_SIZE) { *rc = E_OUT_OF_RANGE; return 0; }
    struct kmem *m = fixed_create(base, npages, KMEM_SCREEN, 0);
    *rc = m ? E_OK : E_NO_MEMORY;
    return m;
}

struct kmem *kmem_create_dma(uint32_t npages, struct job *job, struct kdevice *dev, int *rc) {
    *rc = E_NO_MEMORY;
    if (npages == 0 || npages > DMA_ALLOC_MAX / PAGE_SIZE) { *rc = E_OUT_OF_RANGE; return 0; }
    int r = job_charge_mem(job, (uint64_t)npages * PAGE_SIZE);
    if (r != E_OK) { *rc = r; return 0; }
    paddr_t p = pmm_alloc_pages_low_user(npages);
    /* the low zone's promise, kept as the rule a device relies on */
    if (p && (uint64_t)p + (uint64_t)npages * PAGE_SIZE > 0x100000000ull) {
        pmm_free_pages(p, npages);
        p = 0;
    }
    if (!p) goto no_pages;
    /* Zeroed before anyone can see it: a page off the free list may hold
       another process's memory. */
    for (uint32_t i = 0; i < npages; i++) phys_zero(p + (paddr_t)i * PAGE_SIZE);
    struct kmem *m = fixed_create(p, npages, KMEM_DMA, dev);
    if (!m) { pmm_free_pages(p, npages); goto no_pages; }
    kobj_ref(&job->hdr);
    m->job = job;
    *rc = E_OK;
    return m;

no_pages:
    job_uncharge_mem(job, (uint64_t)npages * PAGE_SIZE);
    return 0;
}

void kmem_dma_release(struct kmem *m) {
    if (m->kind != KMEM_DMA) kpanic("kmem_dma_release: not a DMA buffer");
    pmm_free_pages(m->phys, m->npages);
    job_uncharge_mem(m->job, (uint64_t)m->npages * PAGE_SIZE);
    job_put(m->job);
    kfree(m->pages);
    ocache_free(&mem_cache, m);
}

uint32_t kmem_map_flags(const struct kmem *m) {
    return m->kind == KMEM_DEVICE ? VM_UNCACHED
         : m->kind == KMEM_SCREEN ? VM_WC : 0;
}

paddr_t kmem_page(struct kmem *m, uint32_t index) {
    if (index >= m->npages) kpanic("kmem_page: past the object's end");
    uint32_t f = spin_lock(&m->lock);
    paddr_t p = m->pages[index];
    if (!p) {
        /* Zeroed before anyone can see it: a page off the free list may
           hold another process's memory. */
        p = pmm_alloc_reserved();
        phys_zero(p);
        m->pages[index] = p;
        m->resident++;
    }
    spin_unlock(&m->lock, f);
    return p;
}

paddr_t kmem_page_if_resident(struct kmem *m, uint32_t index) {
    if (index >= m->npages) return 0;
    uint32_t f = spin_lock(&m->lock);
    paddr_t p = m->pages[index];
    spin_unlock(&m->lock, f);
    return p;
}

uint32_t kmem_resident(struct kmem *m) {
    uint32_t f = spin_lock(&m->lock);
    uint32_t n = m->resident;
    spin_unlock(&m->lock, f);
    return n;
}

int kmem_writer_add(struct kmem *m) {
    uint32_t f = spin_lock(&m->lock);
    int rc = (m->seals & MEM_SEAL_WRITE) ? E_ACCESS_DENIED : m->lent ? E_BUSY : E_OK;
    if (rc == E_OK) m->writers++;
    spin_unlock(&m->lock, f);
    return rc;
}

void kmem_writer_drop(struct kmem *m) {
    uint32_t f = spin_lock(&m->lock);
    if (!m->writers) kpanic("kmem_writer_drop: no writer counted");
    m->writers--;
    spin_unlock(&m->lock, f);
}

int kmem_seal(struct kmem *m, uint32_t seals) {
    uint32_t f = spin_lock(&m->lock);
    int rc = (m->seals & MEM_SEAL_SEAL)                ? E_ACCESS_DENIED
           : ((seals & MEM_SEAL_WRITE) && m->writers) ? E_BUSY : E_OK;
    if (rc == E_OK) m->seals |= seals;
    spin_unlock(&m->lock, f);
    return rc;
}

/* ---- leases (2.4) ----
 * A window's buffer, while the display server shows it. A seal cannot do it:
 * a seal never comes off, and a buffer goes back and forth for as long as the
 * window lives. The authority to end it is the lease itself, a handle nobody
 * can copy, so a client cannot end what it has handed over; and it ends with
 * its last reference, so a dead server gives every buffer back. */
struct klease {
    struct kobj hdr;     /* first; KOBJ_LEASE */
    struct kmem *m;      /* a reference, or 0 for one refused as it was made */
};

static void lease_destroy(struct kobj *o) {
    struct klease *l = (struct klease *)o;
    if (l->m) {
        uint32_t f = spin_lock(&l->m->lock);
        l->m->lent = 0;
        spin_unlock(&l->m->lock, f);
        kobj_unref(&l->m->hdr);
    }
    kfree(l);
}

static const struct kobj_ops lease_ops = {
    .name         = "lease",
    .rights_valid = 0,
    .transfer     = 1,
    .destroy      = lease_destroy,
};

struct kobj *kmem_lend(struct kmem *m, int *rc) {
    if (m->kind != KMEM_ANON) { *rc = E_NOT_SUPPORTED; return 0; }
    struct klease *l = kmalloc(sizeof *l);
    if (!l) { *rc = E_NO_MEMORY; return 0; }
    l->m = 0;
    if (kobj_init(&l->hdr, KOBJ_LEASE) != 0) { kfree(l); *rc = E_NO_MEMORY; return 0; }
    uint32_t f = spin_lock(&m->lock);
    int r = (m->writers || m->lent) ? E_BUSY : E_OK;
    if (r == E_OK) m->lent = 1;
    spin_unlock(&m->lock, f);
    if (r != E_OK) { kobj_unref(&l->hdr); *rc = r; return 0; }
    kobj_ref(&m->hdr);
    l->m = m;
    return &l->hdr;
}

int kmem_lease_is_on(struct kobj *lease, struct kmem *m) {
    return ((struct klease *)lease)->m == m;
}

uint32_t kmem_seals(struct kmem *m) {
    uint32_t f = spin_lock(&m->lock);
    uint32_t s = m->seals;
    spin_unlock(&m->lock, f);
    return s;
}
