// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/cxfs_one.c */
/* Aurora Tejeda / CATX Systems */
/* One hostile disk through CXFS, and the rules it is held to (cxfs_one.h). */
#include "cxfs_one.h"
#include "hostdisk.h"

#include "drivers/storage/filesys/cxfs.h"

#include <stdio.h>
#include <string.h>

#define ROOT_DISK   0x10
#define FUZZ_DISK   0x20
#define ROOT_BLOCKS 192u            /* the smallest a format accepts, rounded up */
#define SPB         (CXFS_BLOCK_SIZE / 512u)

#define TAG(v, idx) (((uint32_t)(v) << 24) | (uint32_t)(idx))

/* ---- regions, and the rule for each disk ---- */

enum { R_SB = 1, R_BMP = 2, R_MAN = 4, R_DATA = 8 };

struct watched {
    uint8_t  id;
    uint64_t base;                  /* the volume's first sector */
    uint64_t sectors;               /* the device's size */
    struct cxfs_superblock sb;      /* what the driver will read from block 0 */
    int      reads, writes;         /* R_* allowed in the current step */
    int      accepted;              /* cxfs_sb_validate takes that superblock */
};

static struct watched w[2];
static int  nwatched;
static char verdict[200];
struct cxfs_one_stats cxfs_one_last;
static const char *step;

static void fail(const char *fmt, uint8_t id, uint64_t a, uint64_t b) {
    if (verdict[0]) return;                       /* the first one is the finding */
    char what[120];
    snprintf(what, sizeof what, fmt, (unsigned long long)a, (unsigned long long)b);
    snprintf(verdict, sizeof verdict, "%s: disk %02x: %s", step, id, what);
}

static int region_of(const struct cxfs_superblock *sb, uint64_t blk) {
    if (blk == 0) return R_SB;
    if (blk >= sb->bitmap_start && blk - sb->bitmap_start < sb->bitmap_blocks)
        return R_BMP;
    if (blk >= sb->manifest_start && blk - sb->manifest_start < sb->manifest_blocks)
        return R_MAN;
    if (blk >= sb->data_start && blk < sb->total_blocks) return R_DATA;
    return 0;
}

static void on_transfer(uint8_t id, uint64_t lba, uint32_t count, int write, int rc) {
    (void)rc;                     /* a refused transfer was still asked for */
    struct watched *d = NULL;
    for (int i = 0; i < nwatched; i++) if (w[i].id == id) d = &w[i];
    if (!d) { fail("a disk nothing is mounted from (lba %llu)", id, lba, 0); return; }

    if (lba < d->base) {
        fail("lba %llu is before the volume's first sector %llu", id, lba, d->base);
        return;
    }
    uint64_t rel = lba - d->base;
    /* The mount's probe sector and block 0 are read before anything says how
       big the volume is, so the registry's refusal is the right answer when
       the device is smaller; from there on validation promised it fits. */
    if (!write && rel == 0 && (count == 1 || count == SPB)) {
        if (!(d->reads & R_SB)) fail("superblock read outside a mount (lba %llu)", id, lba, 0);
        return;
    }
    if (count > d->sectors || lba > d->sectors - count) {
        fail("lba %llu count %llu leaves the device", id, lba, count);
        return;
    }
    if (rel % SPB || count != SPB) {
        fail("lba %llu count %llu is not one whole block", id, lba, count);
        return;
    }
    uint64_t blk = rel / SPB;
    if (blk >= d->sb.total_blocks) {
        fail("block %llu is past total_blocks %llu", id, blk, d->sb.total_blocks);
        return;
    }
    int r = region_of(&d->sb, blk);
    int allowed = write ? d->writes : d->reads;
    if (!(r & allowed)) {
        fail(write ? "wrote block %llu (region %llu) it had no reason to"
                   : "read block %llu (region %llu) it had no reason to",
             id, blk, (uint64_t)r);
        return;
    }
    if (r != R_SB && !d->accepted)
        fail("touched block %llu of a volume cxfs_sb_validate refuses (region %llu)",
             id, blk, (uint64_t)r);
}

/* Set what every watched disk may be touched for, from here to the next call. */
static void allow(const char *what, int reads, int writes) {
    step = what;
    for (int i = 0; i < nwatched; i++) { w[i].reads = reads; w[i].writes = writes; }
}

static struct watched *watch(uint8_t id, uint64_t base, uint64_t sectors) {
    struct watched *d = &w[nwatched++];
    memset(d, 0, sizeof *d);
    d->id = id; d->base = base; d->sectors = sectors;
    hd_peek(id, base * 512u, &d->sb, sizeof d->sb);
    d->accepted = cxfs_sb_validate(&d->sb, base, sectors) == 0;
    return d;
}

/* ---- the good root volume a hostile one is mounted beside ---- */

static uint8_t root_image[66 * CXFS_BLOCK_SIZE];    /* sb, bitmap, manifest */
static int     root_made;

static void root_attach(void) {
    hd_attach(ROOT_DISK, (uint64_t)ROOT_BLOCKS * SPB);
    if (!root_made) {
        cxfs_set_disk(ROOT_DISK);
        if (cxfs_format_at(0, ROOT_BLOCKS) != 0) {
            fprintf(stderr, "cxfs_one: the good root volume did not format\n");
            __builtin_trap();
        }
        hd_peek(ROOT_DISK, 0, root_image, sizeof root_image);
        root_made = 1;
        return;
    }
    hd_poke(ROOT_DISK, 0, root_image, sizeof root_image);
}

/* ---- the operations ---- */

#define MAX_SEEN 48
static struct cxfs_entry seen[MAX_SEEN];
static int nseen;
static void collect(const struct cxfs_entry *e) { if (nseen < MAX_SEEN) seen[nseen++] = *e; }

static uint8_t filebuf[64 * 1024];
static uint8_t pattern[9000];

static void run_volume(uint32_t v, const struct cxfs_superblock *sb, int faults) {
    struct cxfs_entry e;
    char path[512];
    uint32_t root = TAG(v, sb->root_id);

    allow("read_entry", R_MAN, 0);
    uint32_t n = sb->manifest_count < 4096 ? sb->manifest_count : 4096;
    for (uint32_t i = 0; i < n + 1; i++) (void)cxfs_read_entry(TAG(v, i), &e);

    allow("list_dir", R_MAN, 0);
    nseen = 0;
    cxfs_list_dir(root, collect);
    cxfs_one_last.listed += nseen;
    /* Breadth first: a directory's entries join the list being walked. */
    for (int i = 0; i < nseen; i++) {
        allow("path_of", R_MAN, 0);
        cxfs_path_of(seen[i].id, path, sizeof path);
        allow("resolve", R_MAN, 0);
        (void)cxfs_resolve(path, 0);
        (void)cxfs_resolve(seen[i].name, root);
        allow("find_in_dir", R_MAN, 0);
        (void)cxfs_find_in_dir(seen[i].parent_id, seen[i].name);
        if (seen[i].type == CXFS_TYPE_DIR) {
            allow("list_dir", R_MAN, 0);
            cxfs_list_dir(seen[i].id, collect);
            (void)cxfs_count_children(seen[i].id);
        } else if (seen[i].type == CXFS_TYPE_FILE) {
            allow("read_file", R_MAN | R_DATA, 0);
            if (cxfs_read_file(seen[i].id, filebuf, sizeof filebuf) >= 0)
                cxfs_one_last.files_read++;
            (void)cxfs_read_at(seen[i].id, 4000, filebuf, 200);
        }
    }

    /* A file of our own: made, written, read back, gone. */
    allow("create", R_MAN | R_BMP | R_DATA, R_MAN | R_BMP | R_DATA);
    int had = cxfs_find_in_dir(root, "fuzz.tmp");
    int id = cxfs_create_entry(root, "fuzz.tmp", CXFS_TYPE_FILE);
    if (id >= 0) {
        allow("write_file", R_MAN | R_BMP | R_DATA, R_MAN | R_BMP | R_DATA);
        int wr = cxfs_write_file((uint32_t)id, pattern, sizeof pattern);
        allow("read back", R_MAN | R_DATA, 0);
        int got = cxfs_read_file((uint32_t)id, filebuf, sizeof filebuf);
        if (wr == 0 && !faults && !verdict[0]) {
            step = "read back";
            if (got != (int)sizeof pattern)
                fail("wrote 9000 bytes, read back %llu", 0, (uint64_t)(uint32_t)got, 0);
            else if (memcmp(filebuf, pattern, sizeof pattern) != 0)
                fail("read back other bytes than were written", 0, 0, 0);
            else
                cxfs_one_last.written_back++;
        }
        allow("delete", R_MAN | R_BMP | R_DATA, R_MAN | R_BMP | R_DATA);
        int del = cxfs_delete_entry((uint32_t)id);
        if (del == 0 && had < 0 && !faults && cxfs_find_in_dir(root, "fuzz.tmp") >= 0) {
            step = "delete";
            fail("a deleted file is still found", 0, 0, 0);
        }
    }

    /* The disk's own files: overwritten, cut, deleted, through its extents. */
    for (int i = 0; i < nseen && i < 4; i++) {
        if (seen[i].type != CXFS_TYPE_FILE) continue;
        allow("write_at", R_MAN | R_BMP | R_DATA, R_MAN | R_BMP | R_DATA);
        (void)cxfs_write_at(seen[i].id, 100, pattern, 5000);
        allow("truncate", R_MAN | R_BMP | R_DATA, R_MAN | R_BMP | R_DATA);
        (void)cxfs_truncate(seen[i].id, 50);
        allow("delete", R_MAN | R_BMP | R_DATA, R_MAN | R_BMP | R_DATA);
        (void)cxfs_delete_entry(seen[i].id);
    }
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void fuzz_attach(const uint8_t *data, size_t size, uint64_t sectors, uint64_t base) {
    hd_attach(FUZZ_DISK, sectors);
    hd_poke(FUZZ_DISK, base * 512u, data + CXFS_ONE_HEADER, size - CXFS_ONE_HEADER);
}

const char *cxfs_one(const uint8_t *data, size_t size) {
    memset(&cxfs_one_last, 0, sizeof cxfs_one_last);
    verdict[0] = '\0';
    if (size < CXFS_ONE_HEADER) return NULL;
    uint64_t sectors = le32(data);
    uint64_t base    = le32(data + 4);
    int      faults  = (int)(data[8] | data[9] << 8);
    if (faults == 0xFFFF) faults = -1;
    for (size_t i = 0; i < sizeof pattern; i++) pattern[i] = (uint8_t)(i * 7 + 3);

    /* 1. As the root volume, with the manifest cache. */
    hd_reset();
    nwatched = 0;
    fuzz_attach(data, size, sectors, base);
    struct watched *f = watch(FUZZ_DISK, base, sectors);
    hd_watch(on_transfer);
    hd_fail_after(faults);
    cxfs_set_disk(FUZZ_DISK);
    allow("mount as root", R_SB | R_BMP | R_MAN, 0);
    int rc = cxfs_mount_at(base);
    if (rc == 0 && !f->accepted)
        fail("mounted a volume cxfs_sb_validate refuses", FUZZ_DISK, 0, 0);
    if (rc == 0) {
        cxfs_one_last.mounted_root = 1;
        run_volume(0, &f->sb, faults >= 0);
    }

    /* 2. As a second volume, beside a good root. */
    hd_reset();
    nwatched = 0;
    root_attach();
    (void)watch(ROOT_DISK, 0, (uint64_t)ROOT_BLOCKS * SPB);
    fuzz_attach(data, size, sectors, base);
    f = watch(FUZZ_DISK, base, sectors);
    hd_watch(on_transfer);
    cxfs_set_disk(ROOT_DISK);
    allow("mount the good root", R_SB | R_BMP | R_MAN, 0);
    if (cxfs_mount_at(0) != 0) {
        step = "mount the good root";
        fail("the good root volume did not mount", ROOT_DISK, 0, 0);
        return verdict;
    }
    hd_fail_after(faults);
    /* The mount point is made on the root, so the root may be written. */
    allow("mount_volume", R_MAN, R_MAN);
    f->reads = R_SB | R_BMP | R_MAN; f->writes = 0;
    int slot = cxfs_mount_volume(FUZZ_DISK, base, TAG(0, 0), NULL);
    if (slot > 0 && !f->accepted)
        fail("mounted a volume cxfs_sb_validate refuses", FUZZ_DISK, 0, 0);
    if (slot > 0) {
        cxfs_one_last.mounted_second = 1;
        run_volume((uint32_t)slot, &f->sb, faults >= 0);
        step = "unmount";
        if (cxfs_unmount_volume((uint32_t)slot) != 0)
            fail("a mounted volume would not unmount", FUZZ_DISK, 0, 0);
    }

    hd_watch(NULL);
    return verdict[0] ? verdict : NULL;
}
