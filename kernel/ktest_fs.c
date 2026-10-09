// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_fs.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Malformed superblocks and corrupt manifest entries for CXFS.
 *
 * The shape is ktest_loader.c's and ktest_net.c's, for the same reasons it
 * works there: build one known-good structure, make every case a SINGLE
 * mutation of it, and keep the known-good structure as a case of its own - so
 * the suite cannot pass by refusing everything, which is the way a validator
 * test fails silently.
 *
 * What these would have caught, had they existed. The superblock of a non-boot
 * disk is untrusted input, because cxk_mount_extra_volumes probes every
 * attached disk at boot and mounts anything whose first sector carries
 * CXFS_MAGIC - so attaching a disk chooses these fields - and cxfs_mount_at
 * checked exactly three of them:
 *
 *   - block_size was checked only for `>= 512`, and it sizes every disk read.
 *     sectors_per_block = block_size / 512 reaches 127 for a uint16_t field,
 *     disk_read's count is a uint32_t, and disk_check bounds lba and count
 *     against the DEVICE and never against the destination buffer. So
 *     read_block(0, &vol->sb) transferred up to 65024 bytes into a 4096-byte
 *     struct, off the end of volumes[] and through `uint8_t *manifest` - a
 *     pointer manifest_load then writes 4KB blocks through.
 *   - Nothing related manifest_count to manifest_blocks, so a count of 1024
 *     with one manifest block made entries 16..1023 come from blocks past the
 *     manifest - data blocks, or another partition - parsed as cxfs_entry, and
 *     written back the same way.
 *   - No extent field was ever checked. The destructive one is the free path:
 *     a corrupt extent_len sent bitmap_flush_span walking far past
 *     bitmap_blocks, writing a zero-filled block over the manifest and the
 *     data region, so one delete destroyed the volume.
 *
 * The extent cases assert on the OUTPUTS rather than only the return value: the
 * volume is still readable afterwards, and a file written before the corruption
 * still has its exact contents. A test that only asked "did it refuse?" would
 * pass on a refusal that had already zeroed the manifest on its way out.
 */

#include <stdint.h>
#include <stddef.h>
#include "ktest_fs.h"
#include "cxfs.h"
#include "string.h"
#include "logging.h"

/* ====================================================================
 * The superblock
 * ==================================================================== */

/* One well-formed superblock: the layout cxfs_format_at computes for a 16MB
   whole-disk volume. 4096 blocks of 4KB; bitmap in block 1; 64 manifest blocks
   holding 1024 entries from block 2; data from block 66. */
#define GOOD_TOTAL_BLOCKS   4096u
#define GOOD_DISK_SECTORS   (GOOD_TOTAL_BLOCKS * 8u)   /* 4KB blocks / 512 */

static void sb_good(struct cxfs_superblock *sb) {
    memset(sb, 0, sizeof *sb);
    sb->magic           = CXFS_MAGIC;
    sb->version         = CXFS_VERSION;
    sb->block_size      = CXFS_BLOCK_SIZE;
    sb->base_lba        = 0;
    sb->total_blocks    = GOOD_TOTAL_BLOCKS;
    sb->bitmap_start    = 1;
    sb->bitmap_blocks   = 1;
    sb->manifest_start  = 2;
    sb->manifest_blocks = 64;
    sb->manifest_count  = 1024;
    sb->data_start      = 66;
    sb->reserved_blocks = 64;
    sb->root_id         = 0;
    sb->entry_size      = CXFS_ENTRY_SIZE;
    sb->feature_flags   = CXFS_FEAT_TIMESTAMPS | CXFS_FEAT_PERMS | CXFS_FEAT_LOCKING;
}

/* accepted / refused, at the volume's real size */
static int accepts(const struct cxfs_superblock *sb) {
    return cxfs_sb_validate(sb, 0, GOOD_DISK_SECTORS) == 0;
}
static int refuses(const struct cxfs_superblock *sb) {
    return cxfs_sb_validate(sb, 0, GOOD_DISK_SECTORS) != 0;
}

int ktest_cxfs_sb_adversarial(void) {
    struct cxfs_superblock sb;
    int ok = 1;

    /* ---- the control ----
       Without this the rest proves nothing: a validator that refuses
       everything passes every refusal case below. */
    sb_good(&sb);
    if (!accepts(&sb)) return 0;

    /* And the same superblock with the device-size test skipped, which is the
       mode the rest of this suite runs in. */
    sb_good(&sb);
    if (cxfs_sb_validate(&sb, 0, 0) != 0) return 0;

    /* The strongest control there is: the superblock of the volume this kernel
       actually booted from has to pass the function that guards mounting. If a
       check below is too strict, this is what says so. */
    if (cxfs_is_mounted()) {
        const struct cxfs_superblock *live = cxfs_get_superblock();
        if (!live) return 0;
        if (cxfs_sb_validate(live, live->base_lba, 0) != 0) return 0;
    }

    /* ---- identity ---- */
    sb_good(&sb); sb.magic   = CXFS_MAGIC ^ 1u;  ok = ok && refuses(&sb);
    sb_good(&sb); sb.magic   = 0;                ok = ok && refuses(&sb);
    sb_good(&sb); sb.version = CXFS_VERSION - 1; ok = ok && refuses(&sb);
    sb_good(&sb); sb.version = CXFS_VERSION + 1; ok = ok && refuses(&sb);

    /* ---- the overflow: block_size sizes the read that fills vol->sb ----
       Every block_size that yields more than the 8 sectors a 4KB block takes
       asked disk_read for a transfer larger than the destination. The whole
       range is covered rather than one value, because the old test was
       `>= 512` and every one of these passed it. */
    for (uint32_t k = 9; k <= 127; k++) {
        sb_good(&sb);
        sb.block_size = (uint16_t)(k * 512u);
        ok = ok && refuses(&sb);
    }
    /* the sharpest single value: 127 sectors, 65024 bytes into 4096 */
    sb_good(&sb); sb.block_size = 65024; ok = ok && refuses(&sb);

    /* Smaller than 4096 passed the old check too, and makes every block
       operation read less than it believes it read. */
    sb_good(&sb); sb.block_size = 512;  ok = ok && refuses(&sb);
    sb_good(&sb); sb.block_size = 1024; ok = ok && refuses(&sb);
    sb_good(&sb); sb.block_size = 2048; ok = ok && refuses(&sb);

    /* and the values either side of the one legal size, plus zero */
    sb_good(&sb); sb.block_size = CXFS_BLOCK_SIZE - 1; ok = ok && refuses(&sb);
    sb_good(&sb); sb.block_size = CXFS_BLOCK_SIZE + 1; ok = ok && refuses(&sb);
    sb_good(&sb); sb.block_size = 0;                   ok = ok && refuses(&sb);

    /* entry_size is the same kind of field: the manifest arithmetic hardcodes
       256, so a volume declaring anything else is not one this code can read. */
    sb_good(&sb); sb.entry_size = CXFS_ENTRY_SIZE / 2; ok = ok && refuses(&sb);
    sb_good(&sb); sb.entry_size = CXFS_ENTRY_SIZE * 2; ok = ok && refuses(&sb);
    sb_good(&sb); sb.entry_size = 0;                   ok = ok && refuses(&sb);

    /* ---- manifest_count against the blocks that hold it ----
       This is the shape that turned arbitrary disk blocks into entries: the
       count is what bounds an index, the blocks are what actually exist. */
    sb_good(&sb); sb.manifest_blocks = 1;   ok = ok && refuses(&sb);  /* 1024 in 16 slots */
    sb_good(&sb); sb.manifest_blocks = 63;  ok = ok && refuses(&sb);  /* one block short */

    /* exactly full is legal: the boundary from the other side */
    sb_good(&sb);
    sb.manifest_blocks = 64; sb.manifest_count = 64 * 16;
    ok = ok && accepts(&sb);
    /* one entry more than the blocks can hold is not */
    sb_good(&sb);
    sb.manifest_blocks = 64; sb.manifest_count = 64 * 16 + 1;
    ok = ok && refuses(&sb);

    /* a smaller manifest, consistent with itself, is fine */
    sb_good(&sb);
    sb.manifest_blocks = 2; sb.manifest_count = 32;
    sb.data_start = 4;
    ok = ok && accepts(&sb);

    sb_good(&sb); sb.manifest_count  = 0;                  ok = ok && refuses(&sb);
    sb_good(&sb); sb.manifest_count  = 1025;               ok = ok && refuses(&sb);
    sb_good(&sb); sb.manifest_count  = 0xFFFFFFFFu;        ok = ok && refuses(&sb);
    sb_good(&sb); sb.manifest_blocks = 0;                  ok = ok && refuses(&sb);

    /* ---- the root has to be a slot that exists ---- */
    sb_good(&sb); sb.root_id = sb.manifest_count;     ok = ok && refuses(&sb);
    sb_good(&sb); sb.root_id = 0xFFFFFFFFu;           ok = ok && refuses(&sb);
    /* the last real slot is legal */
    sb_good(&sb); sb.root_id = sb.manifest_count - 1; ok = ok && accepts(&sb);

    /* ---- region order and overlap ----
       Block 0 is the superblock, then bitmap, then manifest, then data. Each
       region must end at or before the next one starts, which is also what
       bounds every start+len inside the volume. */
    sb_good(&sb); sb.bitmap_start  = 0; ok = ok && refuses(&sb);  /* aliases the sb */
    sb_good(&sb); sb.bitmap_blocks = 0; ok = ok && refuses(&sb);
    /* bitmap running into the manifest */
    sb_good(&sb); sb.bitmap_blocks = 2;  ok = ok && refuses(&sb);
    sb_good(&sb); sb.bitmap_start  = 2;  ok = ok && refuses(&sb);
    /* manifest running into the data region */
    sb_good(&sb); sb.manifest_start = 3;  ok = ok && refuses(&sb);
    sb_good(&sb); sb.manifest_blocks = 65; ok = ok && refuses(&sb);
    /* the unbounded bitmap_blocks that used to drive a disk read per iteration
       in bitmap_load, with no ceiling at all */
    sb_good(&sb); sb.bitmap_blocks = 0xFFFFFFFFu; ok = ok && refuses(&sb);

    /* a gap between regions is not an overlap, and is allowed */
    sb_good(&sb);
    sb.manifest_start = 4; sb.data_start = 80;
    ok = ok && accepts(&sb);

    /* ---- the volume has to contain a data region ---- */
    sb_good(&sb); sb.data_start = sb.total_blocks;     ok = ok && refuses(&sb);
    sb_good(&sb); sb.data_start = sb.total_blocks + 1; ok = ok && refuses(&sb);
    sb_good(&sb); sb.data_start = 0xFFFFFFFFu;         ok = ok && refuses(&sb);
    /* one data block is enough */
    sb_good(&sb);
    sb.data_start = sb.total_blocks - 1; sb.reserved_blocks = 0;
    ok = ok && accepts(&sb);

    /* reserved space cannot exceed the data region */
    sb_good(&sb);
    sb.reserved_blocks = sb.total_blocks - sb.data_start + 1;
    ok = ok && refuses(&sb);
    sb_good(&sb); sb.reserved_blocks = 0xFFFFFFFFu; ok = ok && refuses(&sb);
    /* exactly all of it is legal - a volume with no free space is still valid */
    sb_good(&sb);
    sb.reserved_blocks = sb.total_blocks - sb.data_start;
    ok = ok && accepts(&sb);

    /* ---- the bitmap must describe the blocks addressed through it ----
       Past CXFS_MAX_BITMAP_BYTES the RAM buffer is the limit and bitmap_test
       answers "used" for what it does not cover, so the requirement is only
       that the on-disk bitmap covers as much as the buffer holds.
       Both cases pass disk_sectors 0 deliberately. A 400000-block volume does
       not fit GOOD_DISK_SECTORS, so with the fit test enabled the refusal below
       would prove only that the fit test works and nothing about the bitmap. */
    sb_good(&sb);
    sb.total_blocks = 400000;          /* 50000 bytes of bitmap, 1 block holds 4096 */
    ok = ok && (cxfs_sb_validate(&sb, 0, 0) != 0);
    /* two blocks of bitmap cover the 8192-byte buffer, so a large volume with
       enough bitmap is accepted */
    sb_good(&sb);
    sb.total_blocks = 400000; sb.bitmap_blocks = 2; sb.manifest_start = 3;
    sb.data_start = 67;
    ok = ok && (cxfs_sb_validate(&sb, 0, 0) == 0);

    /* ---- the volume has to fit the device ----
       Without this, "inside total_blocks" bounds nothing, because total_blocks
       could be 0xFFFFFFFF. */
    sb_good(&sb);
    ok = ok && (cxfs_sb_validate(&sb, 0, GOOD_DISK_SECTORS - 1) != 0);
    /* exactly filling the device is legal: the boundary from the other side */
    sb_good(&sb);
    ok = ok && (cxfs_sb_validate(&sb, 0, GOOD_DISK_SECTORS) == 0);
    /* a partition's volume is offset, and the offset counts toward the fit */
    sb_good(&sb);
    ok = ok && (cxfs_sb_validate(&sb, 64, GOOD_DISK_SECTORS + 64) == 0);
    ok = ok && (cxfs_sb_validate(&sb, 64, GOOD_DISK_SECTORS)      != 0);
    /* total_blocks near the top of a uint32_t, where base + total * 8 wraps a
       64-bit sum if it is computed carelessly */
    sb_good(&sb);
    sb.total_blocks = 0xFFFFFFFFu;
    ok = ok && (cxfs_sb_validate(&sb, 0, GOOD_DISK_SECTORS) != 0);
    sb_good(&sb);
    ok = ok && (cxfs_sb_validate(&sb, 0xFFFFFFFFFFFFFF00ull, 0xFFFFFFFFFFFFFFFFull) != 0);

    /* a NULL superblock is a refusal, not a fault */
    ok = ok && (cxfs_sb_validate(NULL, 0, 0) != 0);

    return ok;
}

/* ====================================================================
 * Manifest entries: extents and names
 *
 * These need a mounted volume, and they work by planting a corrupt entry with
 * cxfs_write_entry - which deliberately does not validate extents, because it
 * is also the path that would repair one. Every case restores the entry it
 * corrupted before moving on, so the volume is left as it was found.
 * ==================================================================== */

#define FS_PROBE_DIR   "/Temp"
#define CANARY_TEXT    "cxfs-canary-0123456789"

/* resolve `name` in /Temp, creating it as a file if it is not there */
static int probe_file(const char *name) {
    int dir = cxfs_resolve(FS_PROBE_DIR, 0);
    if (dir < 0) return -1;
    int id = cxfs_find_in_dir((uint32_t)dir, name);
    if (id < 0) id = cxfs_create_entry((uint32_t)dir, name, CXFS_TYPE_FILE);
    return id;
}

/* the canary still reads back exactly, which is the assertion that a refused
   operation did not take the manifest or the data region with it */
static int canary_intact(uint32_t id) {
    char buf[64];
    int n = cxfs_read_file(id, buf, sizeof buf);
    if (n != (int)strlen(CANARY_TEXT)) return 0;
    for (int i = 0; i < n; i++)
        if (buf[i] != CANARY_TEXT[i]) return 0;
    /* and the volume as a whole is still navigable */
    if (cxfs_resolve("/", 0) != 0) return 0;
    if (cxfs_resolve(FS_PROBE_DIR, 0) < 0) return 0;
    return 1;
}

int ktest_cxfs_entry_adversarial(void) {
    if (!cxfs_is_mounted()) return 1;        /* nothing mounted - skip */

    const struct cxfs_superblock *sb = cxfs_get_superblock();
    if (!sb) return 0;
    uint32_t data_start   = sb->data_start;
    uint32_t total_blocks = sb->total_blocks;

    /* A file written before anything is corrupted. Every case below asserts it
       is still intact afterwards - the output, not just the return value. */
    int canary = probe_file("ktfs_canary.txt");
    if (canary < 0) return 0;
    if (cxfs_write_file((uint32_t)canary, CANARY_TEXT, (uint32_t)strlen(CANARY_TEXT)) != 0)
        return 0;
    if (!canary_intact((uint32_t)canary)) return 0;

    /* The victim: a real file with real blocks, whose entry gets bent. */
    int vid = probe_file("ktfs_victim.txt");
    if (vid < 0) return 0;
    if (cxfs_write_file((uint32_t)vid, "victim", 6) != 0) return 0;

    struct cxfs_entry good;
    if (cxfs_read_entry((uint32_t)vid, &good) != 0) return 0;

    int ok = 1;
    char buf[64];

    /* ---- the control: the file reads back before anything is bent ---- */
    if (cxfs_read_file((uint32_t)vid, buf, sizeof buf) != 6) return 0;
    if (cxfs_read_at((uint32_t)vid, 0, buf, 6) != 6) return 0;

    /* ---- extents that address blocks outside the data region ----
       Each is a single mutation of `good`, planted, asserted, restored. The
       addresses go to read_block, which adds base_lba and asks the device, so
       before this an extent could reach the metadata or another partition. */
    struct {
        uint32_t start, len;
    } bad[] = {
        { 0, 1 },                          /* the superblock itself */
        { 1, 1 },                          /* the bitmap */
        { 2, 1 },                          /* the manifest */
        { data_start - 1, 1 },             /* one block below the data region */
        { total_blocks, 1 },               /* one past the end of the volume */
        { total_blocks - 1, 2 },           /* crosses the last valid block */
        { data_start, total_blocks },      /* length past the end */
        { data_start, 0xFFFFFFFFu },       /* the length that zeroed the volume */
        { 0xFFFFFFFFu, 1 },                /* start at the top of the range */
        { 0xFFFFFFF0u, 0x20u },            /* start + len wraps a uint32_t */
    };

    for (unsigned c = 0; c < sizeof bad / sizeof bad[0]; c++) {
        struct cxfs_entry e = good;
        e.extent_start[0] = bad[c].start;
        e.extent_len[0]   = bad[c].len;
        e.size            = CXFS_BLOCK_SIZE;        /* claim there is data there */
        if (cxfs_write_entry(&e) != 0) { ok = 0; break; }

        /* Every read path refuses rather than reading those blocks. */
        ok = ok && (cxfs_read_file((uint32_t)vid, buf, sizeof buf) < 0);
        ok = ok && (cxfs_read_at((uint32_t)vid, 0, buf, 8) < 0);

        /* The write paths refuse at the entry, before any block is freed. */
        ok = ok && (cxfs_truncate((uint32_t)vid, 0) < 0);
        ok = ok && (cxfs_write_at((uint32_t)vid, 0, "z", 1) < 0);
        ok = ok && canary_intact((uint32_t)canary);

        if (cxfs_write_entry(&good) != 0) { ok = 0; break; }   /* restore */

        /* ---- the destructive case, on a sacrificial entry ----
           cxfs_delete_entry does not go through open_for_write, so it is the
           one path that reaches free_extents with a corrupt extent_len - and
           that is where bitmap_flush_span used to walk far past bitmap_blocks
           writing zero-filled blocks over the manifest and the data region. A
           separate file is used because the delete frees the slot.

           The delete is allowed to succeed or fail; neither is the point. The
           assertion is the canary: the volume still has to be there. */
        int sid = probe_file("ktfs_sacrifice.txt");
        if (sid < 0) { ok = 0; break; }
        struct cxfs_entry s;
        if (cxfs_read_entry((uint32_t)sid, &s) != 0) { ok = 0; break; }
        s.extent_start[0] = bad[c].start;
        s.extent_len[0]   = bad[c].len;
        s.size            = CXFS_BLOCK_SIZE;
        if (cxfs_write_entry(&s) != 0) { ok = 0; break; }
        (void)cxfs_delete_entry((uint32_t)sid);
        ok = ok && canary_intact((uint32_t)canary);
    }
    if (cxfs_write_entry(&good) != 0) ok = 0;

    /* ---- the extents that ARE legal, so the checks above are not just
            refusing everything ---- */
    {
        struct cxfs_entry e = good;
        e.extent_start[0] = data_start;          /* the first legal data block */
        e.extent_len[0]   = 1;
        e.size            = 4;
        ok = ok && (cxfs_write_entry(&e) == 0);
        ok = ok && (cxfs_read_at((uint32_t)vid, 0, buf, 4) == 4);

        e.extent_start[0] = total_blocks - 1;    /* the LAST legal data block */
        e.extent_len[0]   = 1;
        ok = ok && (cxfs_write_entry(&e) == 0);
        ok = ok && (cxfs_read_at((uint32_t)vid, 0, buf, 4) == 4);

        /* an empty extent slot is vacuously valid - every file has seven */
        e.extent_start[0] = 0; e.extent_len[0] = 0; e.size = 0;
        ok = ok && (cxfs_write_entry(&e) == 0);
        ok = ok && (cxfs_read_at((uint32_t)vid, 0, buf, 4) == 0);   /* at EOF */

        if (cxfs_write_entry(&good) != 0) ok = 0;
    }

    /* ---- a name that arrives off the disk with no terminator ----
       64 non-zero bytes is representable in the on-disk field, and CXOS's
       strlcpy measures its source before consulting `size`, so cxfs_path_of
       used to read past the name field and build a path out of what followed. */
    {
        struct cxfs_entry e = good;
        for (int i = 0; i < CXFS_NAME_LEN; i++) e.name[i] = 'A';
        e.name_len = CXFS_NAME_LEN;
        if (cxfs_write_entry(&e) != 0) ok = 0;

        struct cxfs_entry back;
        ok = ok && (cxfs_read_entry((uint32_t)vid, &back) == 0);
        ok = ok && (back.name[CXFS_NAME_LEN - 1] == '\0');   /* terminated on the way out */

        /* and the path built from it is bounded and terminated */
        char path[128];
        memset(path, 'x', sizeof path);
        cxfs_path_of((uint32_t)vid, path, (int)sizeof path);
        int term = 0;
        for (unsigned i = 0; i < sizeof path; i++) if (!path[i]) { term = 1; break; }
        ok = ok && term;
        ok = ok && canary_intact((uint32_t)canary);

        if (cxfs_write_entry(&good) != 0) ok = 0;
    }

    /* ---- rename keeps name_len describing the name ---- */
    {
        if (cxfs_rename((uint32_t)vid, "ktfs_renamed.txt") != 0) ok = 0;
        struct cxfs_entry e;
        ok = ok && (cxfs_read_entry((uint32_t)vid, &e) == 0);
        ok = ok && ((int)e.name_len == (int)strlen("ktfs_renamed.txt"));
        /* and an over-long name is still refused rather than truncated */
        char longname[CXFS_NAME_LEN + 8];
        for (unsigned i = 0; i < sizeof longname - 1; i++) longname[i] = 'b';
        longname[sizeof longname - 1] = '\0';
        ok = ok && (cxfs_rename((uint32_t)vid, longname) != 0);
        ok = ok && (cxfs_read_entry((uint32_t)vid, &e) == 0);
        ok = ok && ((int)e.name_len == (int)strlen("ktfs_renamed.txt"));  /* unchanged */
        if (cxfs_rename((uint32_t)vid, "ktfs_victim.txt") != 0) ok = 0;
    }

    /* ---- deleting an entry frees its blocks AND marks the slot free ----
       On a secondary volume this used to fail after the blocks were already
       released, leaving an entry pointing at freed blocks for the next
       allocation to hand to someone else. The tag on parent_id is what fixes
       it, and volume 0's tag is 0 - so this case can only prove the fix where
       a second volume is mounted, and says so rather than implying otherwise. */
    {
        uint32_t v = 0;
        for (uint32_t i = 1; i < cxfs_volume_slots(); i++)
            if (cxfs_volume_mounted(i)) { v = i; break; }

        if (!v) {
            klog("KTEST", SEV_WARN,
                 "cxfs delete-on-secondary-volume untested: only the root volume is mounted");
        } else {
            uint32_t mp = cxfs_volume_mount_point(v);
            int dirv = (int)mp;
            int fid = cxfs_find_in_dir((uint32_t)dirv, "ktfs_del.txt");
            if (fid < 0) fid = cxfs_create_entry((uint32_t)dirv, "ktfs_del.txt",
                                                 CXFS_TYPE_FILE);
            if (fid < 0) {
                ok = 0;
            } else {
                ok = ok && (cxfs_write_file((uint32_t)fid, "x", 1) == 0);
                /* the delete must SUCCEED, and the slot must actually be free */
                ok = ok && (cxfs_delete_entry((uint32_t)fid) == 0);
                struct cxfs_entry e;
                ok = ok && (cxfs_read_entry((uint32_t)fid, &e) == 0);
                ok = ok && (e.type == CXFS_TYPE_FREE);
                ok = ok && (cxfs_find_in_dir((uint32_t)dirv, "ktfs_del.txt") < 0);
            }
        }
    }

    /* leave the volume as it was found */
    (void)cxfs_truncate((uint32_t)vid, 0);
    ok = ok && canary_intact((uint32_t)canary);
    return ok;
}
