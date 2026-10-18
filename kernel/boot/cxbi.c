// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/boot/cxbi.c */
/* Aurora Tejeda / CATX Systems */
/* Taking the loader's handover (see cxbi.h). */

#include "boot/cxbi.h"
#include "arch/paging.h"   /* KERNEL_VBASE */
#include "panic.h"

/* Before paging_init the only physical memory the kernel can read is the low
   window the entry stub maps at KERNEL_VBASE (arch/x86_32/entry.asm); the
   loader keeps the handover and its map inside it. */
#define BOOT_WINDOW    0x400000u
#define CXBI_MAX_MMAP  128u

static struct cxk_boot_info  info;
static struct cxk_mmap_entry map[CXBI_MAX_MMAP];
static uint32_t map_n, map_dropped;

/* The kernel's view of [phys, phys+len) in the boot window, or 0 if it is not
   wholly inside. */
static const void *boot_window(uint64_t phys, uint32_t len) {
    if (phys == 0 || phys >= BOOT_WINDOW || len > BOOT_WINDOW - phys) return 0;
    return (const void *)(uintptr_t)((uint32_t)phys + KERNEL_VBASE);
}

void cxbi_init(uintptr_t phys) {
    const struct cxk_boot_info *b = boot_window(phys, sizeof *b);
    if (!b) kpanic("CXBI: the loader passed no handover");
    if (b->magic != CXBI_MAGIC)
        kpanic("CXBI: the loader's handover has no CXBI magic");
    if (b->version != CXBI_VERSION || b->size < sizeof *b)
        kpanic("CXBI: a handover version this kernel does not understand");
    info = *b;

    if (info.mmap_entries == 0 || info.mmap_entry_size < sizeof(struct cxk_mmap_entry))
        kpanic("CXBI: the handover has no memory map");
    map_n = info.mmap_entries;
    if (map_n > CXBI_MAX_MMAP) { map_dropped = map_n - CXBI_MAX_MMAP; map_n = CXBI_MAX_MMAP; }
    for (uint32_t i = 0; i < map_n; i++) {
        const struct cxk_mmap_entry *e = boot_window(
            info.mmap_addr + (uint64_t)i * info.mmap_entry_size, sizeof *e);
        if (!e) kpanic("CXBI: the memory map lies outside low memory");
        /* The loader normalises every firmware type into 1..7, so anything
           else means this is not a map at all - most likely a pointer at
           something else. Trusting it starves or corrupts the PMM, which
           shows up as a hang far from here. */
        if (e->type < CXBI_MEM_USABLE || e->type > CXBI_MEM_FRAMEBUFFER)
            kpanic("CXBI: the memory map has an entry of no known type");
        map[i] = *e;
    }
}

const struct cxk_boot_info *cxbi(void)          { return &info; }
uint32_t cxbi_mmap_count(void)                  { return map_n; }
const struct cxk_mmap_entry *cxbi_mmap(uint32_t i) { return i < map_n ? &map[i] : 0; }
uint32_t cxbi_mmap_dropped(void)                { return map_dropped; }
