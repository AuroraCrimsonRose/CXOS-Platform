// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/object/kmem.c */
/* Aurora Tejeda / CATX Systems */
/* Memory objects: see kmem.h. */
#include "object/kmem.h"
#include "memman/ocache.h"
#include "memman/heap.h"
#include "pmm.h"
#include "job.h"
#include "string.h"
#include "cxk_abi.h"
#include "panic.h"

_Static_assert(KOBJ_MEM == OBJ_TYPE_MEM, "object_basic.kind is the kernel type");

static struct ocache mem_cache;

/* Its pages, what was reserved for the rest, and the job's charge. Nothing
   maps it any more: every mapping held a reference. */
static void mem_destroy(struct kobj *o) {
    struct kmem *m = (struct kmem *)o;
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
    .rights_valid = HRIGHT_INSPECT | HRIGHT_MAP | HRIGHT_MAP_WRITE | HRIGHT_MAP_EXEC,
    /* travels with 1.3e, with sealing: until then a receiver could not
       trust what another process can still write */
    .transfer     = 0,
    .destroy      = mem_destroy,
};

void kmem_sys_init(void) {
    if (ocache_init(&mem_cache, "memory", sizeof(struct kmem)) != 0 ||
        kobj_type_register(KOBJ_MEM, &mem_ops) != 0)
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
    m->lock     = (spinlock_t)SPINLOCK_INIT;
    m->npages   = npages;
    m->resident = 0;
    kobj_ref(&job->hdr);
    m->job      = job;
    *rc = E_OK;
    return m;

no_object:
    pmm_unreserve(npages);
no_pages:
    job_uncharge_mem(job, (uint64_t)npages * PAGE_SIZE);
    return 0;
}

paddr_t kmem_page(struct kmem *m, uint32_t index) {
    if (index >= m->npages) kpanic("kmem_page: past the object's end");
    uint32_t f = spin_lock(&m->lock);
    paddr_t p = m->pages[index];
    if (!p) {
        /* Zeroed before anyone can see it: a page off the free list may
           hold another process's memory. */
        p = pmm_alloc_reserved();
        memset(paging_temp_map(p), 0, PAGE_SIZE);
        paging_temp_unmap();
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
