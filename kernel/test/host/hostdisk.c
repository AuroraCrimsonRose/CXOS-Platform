// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/hostdisk.c */
/* Aurora Tejeda / CATX Systems */
/*
 * The storage seam (hostdisk.h): the disk registry's interface, over RAM.
 *
 * Also here, because CXFS is the only kernel code on the host that reaches
 * them: the caller's uid (SYSTEM, so permission checks pass and the paths
 * behind them run) and the wall clock (none, which CXFS already handles as
 * timestamp 0).
 */
#include "hostdisk.h"

#include "drivers/storage/hardware/disk.h"
#include "cpu/uid.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <string.h>

#define PAGE 4096u
#define SECTORS_PER_PAGE (PAGE / 512u)

struct hd_page { uint64_t idx; uint8_t *data; };

struct hd {
    int             used;
    struct disk     reg;          /* what disk_find_by_id hands back */
    struct hd_page *pages;        /* open addressing on idx; data NULL = empty */
    size_t          cap, count;
};

static struct hd   disks[HD_MAX_DISKS];
static hd_watch_fn watcher;
static int         fail_after = -1;
static int         lock_depth;

static struct hd *find(uint8_t id) {
    for (int i = 0; i < HD_MAX_DISKS; i++)
        if (disks[i].used && disks[i].reg.id == id) return &disks[i];
    return NULL;
}

static size_t slot_of(const struct hd *d, uint64_t idx) {
    size_t s = (size_t)((idx * 0x9E3779B97F4A7C15ull) >> 32) & (d->cap - 1);
    while (d->pages[s].data && d->pages[s].idx != idx) s = (s + 1) & (d->cap - 1);
    return s;
}

static uint8_t *page(struct hd *d, uint64_t idx, int make) {
    if (d->cap) {
        size_t s = slot_of(d, idx);
        if (d->pages[s].data) return d->pages[s].data;
        if (!make) return NULL;
    } else if (!make) {
        return NULL;
    }
    if ((d->count + 1) * 4 > d->cap * 3) {            /* grow at 3/4 */
        struct hd_page *old = d->pages;
        size_t oldcap = d->cap;
        d->cap = oldcap ? oldcap * 2 : 64;
        d->pages = calloc(d->cap, sizeof *d->pages);
        if (!d->pages) abort();
        for (size_t i = 0; i < oldcap; i++)
            if (old[i].data) d->pages[slot_of(d, old[i].idx)] = old[i];
        free(old);
    }
    size_t s = slot_of(d, idx);
    d->pages[s].idx  = idx;
    d->pages[s].data = calloc(1, PAGE);
    if (!d->pages[s].data) abort();
    d->count++;
    return d->pages[s].data;
}

static void drop(struct hd *d) {
    for (size_t i = 0; i < d->cap; i++) free(d->pages[i].data);
    free(d->pages);
    memset(d, 0, sizeof *d);
}

void hd_reset(void) {
    for (int i = 0; i < HD_MAX_DISKS; i++) if (disks[i].used) drop(&disks[i]);
    watcher = NULL;
    fail_after = -1;
}

int hd_attach(uint8_t id, uint64_t sectors) {
    if (find(id)) return -1;
    for (int i = 0; i < HD_MAX_DISKS; i++) {
        if (disks[i].used) continue;
        memset(&disks[i], 0, sizeof disks[i]);
        disks[i].used        = 1;
        disks[i].reg.id      = id;
        disks[i].reg.sectors = sectors;
        return 0;
    }
    return -1;
}

static void copy_bytes(struct hd *d, uint64_t off, uint8_t *mem, size_t len, int to_disk) {
    uint64_t end = d->reg.sectors * 512u;
    while (len > 0 && off < end) {
        uint64_t idx = off / PAGE;
        size_t   in  = (size_t)(off % PAGE);
        size_t   n   = PAGE - in;
        if (n > len) n = len;
        if ((uint64_t)n > end - off) n = (size_t)(end - off);
        uint8_t *p = page(d, idx, to_disk);
        if (to_disk)  memcpy(p + in, mem, n);
        else if (p)   memcpy(mem, p + in, n);
        else          memset(mem, 0, n);
        off += n; mem += n; len -= n;
    }
}

void hd_poke(uint8_t id, uint64_t off, const void *src, size_t len) {
    struct hd *d = find(id);
    if (d) copy_bytes(d, off, (uint8_t *)(uintptr_t)src, len, 1);
}

void hd_peek(uint8_t id, uint64_t off, void *dst, size_t len) {
    struct hd *d = find(id);
    if (d) copy_bytes(d, off, dst, len, 0);
    else   memset(dst, 0, len);
}

void hd_watch(hd_watch_fn fn) { watcher = fn; }
void hd_fail_after(int n)     { fail_after = n; }

/* ---- the registry's interface (disk.h) ---- */

const struct disk *disk_find_by_id(uint8_t id) {
    struct hd *d = find(id);
    return d ? &d->reg : NULL;
}

/* disk.c's disk_check, the same rule in the same order: the code under test
   must meet the refusals it meets in the kernel. */
static int check(const struct hd *d, uint64_t lba, uint32_t count, const void *buf) {
    if (!d)              return DISK_ERR_NO_DEVICE;
    if (!buf)            return DISK_ERR_PARAMS;
    if (count == 0)      return DISK_ERR_PARAMS;
    if (lba >= d->reg.sectors)                  return DISK_ERR_BOUNDS;
    if ((uint64_t)count > d->reg.sectors - lba) return DISK_ERR_BOUNDS;
    return DISK_OK;
}

static int transfer(uint8_t id, uint64_t lba, uint32_t count, void *buf, int write) {
    struct hd *d = find(id);
    int rc = check(d, lba, count, buf);
    if (rc == DISK_OK && fail_after >= 0) {
        if (fail_after == 0) rc = DISK_ERR_FAULT;
        else                 fail_after--;
    }
    if (watcher) watcher(id, lba, count, write, rc);
    if (rc != DISK_OK) return rc;
    copy_bytes(d, lba * 512u, buf, (size_t)count * 512u, write);
    return DISK_OK;
}

int disk_read(uint8_t id, uint64_t lba, uint32_t count, void *buf) {
    return transfer(id, lba, count, buf, 0);
}

int disk_write(uint8_t id, uint64_t lba, uint32_t count, const void *buf) {
    return transfer(id, lba, count, (void *)(uintptr_t)buf, 1);
}

int disk_flush(uint8_t id) { return find(id) ? DISK_OK : DISK_ERR_NO_DEVICE; }

/* One thread on the host: the lock only counts, and an unlock without a lock
   is the bug it would be in the kernel. */
void storage_lock(void)   { lock_depth++; }
void storage_unlock(void) { if (--lock_depth < 0) abort(); }
int  storage_held(void)   { return lock_depth > 0; }
void storage_break(void)  { lock_depth = 0; }

/* ---- the rest CXFS reaches ---- */

uid_t current_uid(void) { return 0; }                       /* SYSTEM */

int platform_wallclock(struct datetime *out) { (void)out; return -1; }
