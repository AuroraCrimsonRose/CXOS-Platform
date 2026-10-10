// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /CXLite/kernel/filesys/cxfs.c */
/* Aurora Tejeda */
/* CXFS v1 - format and mount. */

#include "cxfs.h"
#include "disk.h"
#include "string.h"
#include "datetime.h"
#include "rtc.h"
#include "uid.h"
#include "sched.h"

/* current wall-clock time as epoch seconds, from the RTC. 0 if unavailable. */
static uint64_t cxfs_now(void) {
    struct rtc_time t;
    rtc_read(&t);
    struct datetime dt = { t.year, t.month, t.day, t.hour, t.minute, t.second };
    return datetime_to_epoch(&dt);
}

/* Permission check for `access` (CXFS_ACC_*) by the current process.
   SYSTEM (uid 0) always passes. The owner is checked against the owner bits,
   everyone else against the "other" bits (no group membership model yet).
   Returns 1 if allowed, 0 if denied. */
static int cxfs_check_perm(const struct cxfs_entry *e, int access) {
    uid_t uid = current_uid();
    if (uid == UID_SYSTEM) return 1;            /* SYSTEM bypasses checks */

    uint16_t p = e->permissions;
    uint16_t need;
    if (uid == e->owner_uid) {
        need = (access == CXFS_ACC_WRITE) ? CXFS_PERM_OW
             : (access == CXFS_ACC_EXEC)  ? CXFS_PERM_OX : CXFS_PERM_OR;
    } else {
        need = (access == CXFS_ACC_WRITE) ? CXFS_PERM_TW
             : (access == CXFS_ACC_EXEC)  ? CXFS_PERM_TX : CXFS_PERM_TR;
    }
    return (p & need) ? 1 : 0;
}

/* --- v2 advisory locking ("in use, don't edit") ---
   A cooperating writer locks a file (recording its pid); other cooperating
   writers honor it. Advisory only: not enforced against non-cooperating code.
   A lock whose owner pid is no longer a live thread is STALE and ignored. */

/* is `e` effectively locked by someone OTHER than the current process?
   Clears the stale flag in *e (caller may persist it) if the owner is dead. */
static int cxfs_lock_blocks(struct cxfs_entry *e) {
    if (!e->lock_state) return 0;                       /* not locked */
    if (!thread_is_alive((int)e->lock_owner_pid)) {     /* stale -> clear */
        e->lock_state = 0;
        e->lock_owner_pid = 0;
        return 0;
    }
    if ((uint32_t)thread_current_id() == e->lock_owner_pid) return 0; /* ours */
    return 1;                                           /* held by another live proc */
}

/* ====================================================================
 * Mounted volumes
 *
 * Everything that describes ONE mounted filesystem lives in a cxfs_volume:
 * which disk it is on, where on that disk, its superblock, and its caches.
 * These were all file-scope globals, which is the same thing written so that
 * only one can exist. Collecting them is what makes a second mount possible;
 * `vol` points at the one being operated on and every reference below goes
 * through it.
 *
 * Volume 0 is the ROOT volume - the one the system booted from, the one "/"
 * means. It is the only volume that gets the 256KB manifest cache, because
 * that is where essentially all the traffic is and four of them would be a
 * megabyte of permanently resident RAM. A volume without the cache is not
 * broken, just slower: manifest_fetch() already falls back to reading the
 * manifest off the disk, and that fallback is now load-bearing rather than
 * theoretical. The bitmap is 8KB and every volume gets its own, because
 * bitmap_test() has no disk fallback - an unloaded bitmap reads as all-zero,
 * every block looks free, and the volume is destroyed on the first write.
 * ==================================================================== */

#define CXFS_MAX_VOLUMES      4
#define CXFS_MAX_BITMAP_BYTES 8192   /* 32768 blocks / 8 = 4096; headroom */
#define CXFS_MAX_ENTRIES      1024   /* manifest capacity */
#define CXFS_MAX_MANIFEST_BYTES (CXFS_MAX_ENTRIES * CXFS_ENTRY_SIZE)   /* 256KB */

/* Entries per manifest block (16). Up here with the capacities because
   cxfs_sb_validate needs it to check manifest_count against manifest_blocks,
   and that runs before anything else in the file. */
#define ENTRIES_PER_BLOCK (CXFS_BLOCK_SIZE / sizeof(struct cxfs_entry))

/* One manifest cache, handed to the root volume below. See the comment above
   for why the other volumes do without one rather than getting one each. */
static uint8_t root_manifest_cache[CXFS_MAX_MANIFEST_BYTES];

struct cxfs_volume {
    int      mounted;
    uint8_t  disk_id;                 /* registry ID of the disk it lives on */
    uint64_t base_lba;                /* partition offset, sectors */
    uint32_t sectors_per_block;       /* CXFS_BLOCK_SIZE / 512 */
    struct cxfs_superblock sb;

    uint8_t  bitmap[CXFS_MAX_BITMAP_BYTES];
    uint32_t bitmap_bytes;
    int      bitmap_loaded;

    uint8_t *manifest;                /* the cache, or NULL for an uncached volume */
    uint32_t manifest_blocks_cached;  /* 0 = uncached, use the disk */

    char     label[CXFS_LABEL_LEN];   /* the name it answers to */
    uint32_t mount_point;             /* tagged id of the DIRECTORY it is
                                         mounted on, on an already mounted
                                         volume. Unused for the root volume,
                                         which is not mounted on anything. */
    int      placed;                  /* 1 once mount_point is real. Without
                                         this, a volume mid-mount has
                                         mount_point 0 - which is the ROOT
                                         volume's root - and cross_mount would
                                         send every absolute path to it. */
};

static struct cxfs_volume  volumes[CXFS_MAX_VOLUMES] = {
    [0] = { .manifest = root_manifest_cache },   /* the root volume, and only it */
};
static struct cxfs_volume *vol = &volumes[0];   /* the one being operated on */

/* ---- volume-tagged entry ids ----
 *
 * An id handed out by this driver carries the volume it belongs to:
 *
 *     id = (volume << 24) | index
 *
 * The index is a manifest slot, bounded by sb.manifest_count - 1024 today -
 * so 24 bits is far more room than the format can use. The root volume is 0,
 * which makes every root id identical to the untagged id it was before: no
 * stored id changes value, file_stat.id keeps its meaning and its width, and
 * nothing already written to a disk has to be migrated.
 *
 * ON DISK, the id and parent_id inside a cxfs_entry stay volume-LOCAL. A disk
 * must not record where it happens to be mounted - move it to another slot, or
 * mount it somewhere else, and every reference on it would point at a volume
 * that is not this one. The tag goes on when an entry is read and comes off
 * when one is written, so the whole boundary is cxfs_read_entry and
 * cxfs_write_entry and nothing else has to think about it.
 */
#define VOL_SHIFT       24
#define VOL_OF(id)      ((uint32_t)(id) >> VOL_SHIFT)
#define IDX_OF(id)      ((uint32_t)(id) & ((1u << VOL_SHIFT) - 1))
#define TAG_ID(v, idx)  (((uint32_t)(v) << VOL_SHIFT) | (uint32_t)(idx))

/* Point `vol` at the volume `id` names. Returns 0 if that volume is not
 * mounted, in which case `vol` is left alone.
 *
 * There is no matching "restore". These functions call each other - resolve
 * into read_entry, truncate into write_entry - but every id in one of those
 * call trees comes from the same volume, so the nested select is a no-op. The
 * two places where ids from different volumes could meet are cxfs_move and
 * cxfs_is_ancestor, and both reject the cross-volume case outright: a parent
 * pointer cannot move a file between filesystems, that needs a copy.
 *
 * The functions that take no id are what this leaves. cxfs_is_mounted and
 * cxfs_free_blocks answer for the ROOT volume explicitly, because "is the
 * filesystem up" must not depend on which volume was touched last. The
 * allocators are current-volume on purpose: they run underneath a write, and
 * that write has already selected the volume it is writing to.
 */
/* If `id` is a directory that something is mounted on, answer with that
 * volume's root instead. This one function is what makes a mount point behave
 * like the thing mounted on it: every path walk goes through it, so nothing
 * else in the resolver has to know that volumes exist - and nothing is special
 * about /Drives, it is just where the system happens to put them.
 */
static uint32_t cross_mount(uint32_t id) {
    for (uint32_t v = 1; v < CXFS_MAX_VOLUMES; v++)
        if (volumes[v].mounted && volumes[v].placed && volumes[v].mount_point == id)
            return TAG_ID(v, volumes[v].sb.root_id);
    return id;
}

/* The reverse: if `id` is the root of a mounted volume, answer with the
 * directory it is mounted on. Walking up out of a volume needs this, because
 * a volume root's parent is itself and would otherwise be a dead end. */
static uint32_t uncross_mount(uint32_t id) {
    uint32_t v = VOL_OF(id);
    if (v == 0 || v >= CXFS_MAX_VOLUMES) return id;
    if (!volumes[v].mounted || !volumes[v].placed) return id;
    if (id != TAG_ID(v, volumes[v].sb.root_id)) return id;
    return volumes[v].mount_point;
}

static int vol_select(uint32_t id) {
    uint32_t v = VOL_OF(id);
    if (v >= CXFS_MAX_VOLUMES || !volumes[v].mounted) return 0;
    vol = &volumes[v];
    return 1;
}

void    cxfs_set_id(uint8_t id) { vol->disk_id = id; }
uint8_t cxfs_get_id(void)       { return vol->disk_id; }

/* backwards-compatible shims (some callers still use these names) */
void cxfs_set_disk(uint8_t drive) { vol->disk_id = drive; }
uint8_t cxfs_get_disk(void) { return vol->disk_id; }

static int disk_present(void) {
    return disk_find_by_id(vol->disk_id) != 0;
}

/* layout planning constants for a format (CXFS_MAX_ENTRIES is up with the
   volume record, which sizes its manifest cache from it) */
#define CXFS_RESERVED_KB   256             /* system-reserved data region */


static void bitmap_load(void);             /* fwd decl (defined in allocator section) */
static void manifest_load(void);            /* fwd decl (defined in allocator section) */

/* ---- block staging buffers ------------------------------------------------
 * Every CXFS block operation needs a CXFS_BLOCK_SIZE (4KB) staging buffer.
 * These used to be locals, which made the frames enormous: cxfs_write_file's
 * frame measured 4432 bytes and it calls cxfs_write_entry (4160) - 8592 bytes
 * of stack against the 8192-byte default stack that thread_alloc_kstack hands a
 * ring-3 process for its syscall (esp0) stack. The file paths only ever ran on
 * kmain's larger .bss stack, so the overrun stayed invisible until file
 * syscalls put them on a thread stack. Statics keep them out of frames.
 *
 * One buffer per ROLE, so a function that stages a block and then calls
 * another that does the same cannot clobber it. The nesting that actually
 * happens: file data -> entry -> bitmap, and dir iteration -> entry.
 *
 * Safe because CXFS is entered serially: preemption is enabled only around
 * ktest's contained cases (see sched.h), never system-wide, so two threads are
 * never inside CXFS at once. If preemption ever goes system-wide these need a
 * lock, and this comment is the place that says so.
 */
static uint8_t bmp_blk[CXFS_BLOCK_SIZE];   /* allocation bitmap */
static uint8_t ent_blk[CXFS_BLOCK_SIZE];   /* manifest entry read/modify/write */
static uint8_t dir_blk[CXFS_BLOCK_SIZE];   /* directory iteration (callbacks may stat) */
static uint8_t dat_blk[CXFS_BLOCK_SIZE];   /* file contents */

/* read/write one CXFS block via the registry. A CXFS block is
   (block_size/512) sectors, placed at base_lba + block*sectors_per_block, so a
   volume works identically whole-disk or inside a partition. These are set at
   mount/format from the superblock; defaults keep v1 behavior (512B, base 0). */

static int read_block(uint32_t block, void *buf) {
    uint64_t lba = vol->base_lba + (uint64_t)block * vol->sectors_per_block;
    return disk_read(vol->disk_id, lba, (uint8_t)vol->sectors_per_block, buf);
}
static int write_block(uint32_t block, const void *buf) {
    uint64_t lba = vol->base_lba + (uint64_t)block * vol->sectors_per_block;
    return disk_write(vol->disk_id, lba, (uint8_t)vol->sectors_per_block, buf);
}

/* ---- the superblock is untrusted input -----------------------------------
 *
 * cxk_mount_extra_volumes probes every non-boot disk at boot - the whole drive
 * first, then every partition an XBPT table declares - and mounts anything
 * whose first sector carries CXFS_MAGIC and version 2. So attaching a disk is
 * enough to hand this driver a superblock an attacker wrote, and before this
 * function exactly three fields were ever checked: magic, version, and
 * block_size >= 512. Everything else was adopted as true and used to address
 * blocks.
 *
 * What that cost, and why each check below is here rather than at the point of
 * use (2026-10-09 review §3):
 *
 *   - block_size sized every disk read. sectors_per_block = block_size / 512
 *     reaches 127 for a uint16_t field, disk_read's count is a uint32_t, and
 *     disk_check bounds lba and count against the DEVICE and never against the
 *     destination. read_block(0, &vol->sb) therefore transferred up to 65024
 *     bytes into a 4096-byte struct, running off the end of volumes[] and
 *     through `uint8_t *manifest` - a pointer manifest_load then writes 4KB
 *     blocks through. A crafted superblock meant a controlled kernel pointer.
 *     block_size == CXFS_BLOCK_SIZE is the check that closes it, and it has to
 *     be made on the PROBE value, before it sizes a read.
 *
 *   - Nothing related manifest_count to manifest_blocks. cxfs_read_entry bounds
 *     the index against manifest_count alone, so count 1024 with blocks 1 made
 *     entries 16..1023 come from blocks past the manifest - data blocks, or
 *     another partition - parsed as cxfs_entry. cxfs_write_entry took the same
 *     path and WROTE there.
 *
 *   - bitmap_blocks drove a disk read per iteration in bitmap_load with no
 *     ceiling, so a large value stalled the boot inside mount.
 *
 * `disk_sectors` is the device's capacity, or 0 to skip the does-it-fit test -
 * which is what keeps this a pure function the adversarial suite can call with
 * a struct in RAM and no disk at all.
 */
int cxfs_sb_validate(const struct cxfs_superblock *sb, uint64_t base_lba,
                     uint64_t disk_sectors) {
    if (!sb) return -1;

    /* identity */
    if (sb->magic   != CXFS_MAGIC)      return -1;
    if (sb->version != CXFS_VERSION)    return -1;

    /* Geometry is not negotiable: this driver is compiled for 4KB blocks and
       256-byte entries - the staging buffers, ENTRIES_PER_BLOCK, and all the
       bitmap and manifest arithmetic hardcode them. The header already calls
       block_size authoritative; this is what makes that true. */
    if (sb->block_size != CXFS_BLOCK_SIZE)  return -1;
    if (sb->entry_size != CXFS_ENTRY_SIZE)  return -1;

    /* The manifest must be describable by the slots that exist to hold it, and
       must fit the cache's capacity. */
    if (sb->manifest_count < 1)                     return -1;
    if (sb->manifest_count > CXFS_MAX_ENTRIES)      return -1;
    if (sb->manifest_blocks < 1)                    return -1;
    if ((uint64_t)sb->manifest_count >
        (uint64_t)sb->manifest_blocks * ENTRIES_PER_BLOCK) return -1;

    /* Region order, with no overlap. Each test is "this region ends at or
       before the next one starts", which also bounds every start+len inside
       total_blocks once data_start is checked against it below. Written as
       additions on uint64_t so a field near 2^32 cannot wrap past a ceiling. */
    if (sb->bitmap_start < 1)                       return -1;  /* block 0 is the sb */
    if (sb->bitmap_blocks < 1)                      return -1;
    if ((uint64_t)sb->bitmap_start + sb->bitmap_blocks > sb->manifest_start)
        return -1;
    if ((uint64_t)sb->manifest_start + sb->manifest_blocks > sb->data_start)
        return -1;
    if (sb->data_start >= sb->total_blocks)         return -1;  /* no data at all */
    if ((uint64_t)sb->reserved_blocks > (uint64_t)sb->total_blocks - sb->data_start)
        return -1;

    /* The bitmap must actually describe the blocks that will be addressed
       through it. Past CXFS_MAX_BITMAP_BYTES the RAM buffer is the limit and
       bitmap_test answers "used" for anything it does not cover, which is safe
       - so the requirement is only that the on-disk bitmap covers as much as
       the buffer will hold. */
    uint64_t need_bytes = ((uint64_t)sb->total_blocks + 7) / 8;
    if (need_bytes > CXFS_MAX_BITMAP_BYTES) need_bytes = CXFS_MAX_BITMAP_BYTES;
    if ((uint64_t)sb->bitmap_blocks * CXFS_BLOCK_SIZE < need_bytes) return -1;

    /* The root has to be a slot that exists, or every path walk starts nowhere. */
    if (sb->root_id >= sb->manifest_count)          return -1;

    /* And the volume has to fit the device it claims to be on. Without this,
       "inside total_blocks" bounds nothing, because total_blocks itself could
       be 0xFFFFFFFF. */
    if (disk_sectors) {
        uint64_t spb  = sb->block_size / 512;
        uint64_t last = base_lba + (uint64_t)sb->total_blocks * spb;
        if (last < base_lba)        return -1;      /* 64-bit wrap */
        if (last > disk_sectors)    return -1;
    }
    return 0;
}

/* the capacity of the disk the current volume sits on, or 0 if unknown */
static uint64_t vol_disk_sectors(void) {
    const struct disk *d = disk_find_by_id(vol->disk_id);
    return d ? d->sectors : 0;
}

/* The label for the NEXT format, set by cxfs_format_labeled. A parameter would
   be better, but cxfs_format_at is called from several places that have no
   label to give and the signature is in the header; this keeps those callers
   unchanged and is cleared on use so a label can never leak into a later
   format. */
static char format_label[CXFS_LABEL_LEN];

int cxfs_format_at(uint64_t base_lba, uint32_t total_blocks) {
    if (!disk_present()) return -1;

    /* --- figure out the layout (total_blocks = this volume's size in blocks) --- */

    uint32_t manifest_entries = CXFS_MAX_ENTRIES;
    uint32_t entries_per_block = CXFS_BLOCK_SIZE / sizeof(struct cxfs_entry); /* 16 */
    uint32_t manifest_blocks = (manifest_entries + entries_per_block - 1) / entries_per_block;

    uint32_t bitmap_start = 1;             /* right after superblock */
    /* one bit per block; bitmap covers total_blocks bits */
    uint32_t bitmap_bytes = (total_blocks + 7) / 8;
    uint32_t bitmap_blocks = (bitmap_bytes + CXFS_BLOCK_SIZE - 1) / CXFS_BLOCK_SIZE;

    uint32_t manifest_start = bitmap_start + bitmap_blocks;
    uint32_t data_start = manifest_start + manifest_blocks;
    uint32_t reserved_blocks = (CXFS_RESERVED_KB * 1024) / CXFS_BLOCK_SIZE;

    /* v2: this volume's block geometry + placement. base_lba 0 = whole disk
       (dev). Set the I/O globals so read_block/write_block translate correctly. */
    vol->sectors_per_block = CXFS_BLOCK_SIZE / 512;   /* 8 */
    vol->base_lba          = base_lba;

    /* --- build and write the superblock --- */
    memset(&vol->sb, 0, sizeof vol->sb);
    vol->sb.magic           = CXFS_MAGIC;
    vol->sb.version         = CXFS_VERSION;
    vol->sb.block_size      = CXFS_BLOCK_SIZE;
    vol->sb.base_lba        = vol->base_lba;
    vol->sb.total_blocks    = total_blocks;
    vol->sb.bitmap_start    = bitmap_start;
    vol->sb.bitmap_blocks   = bitmap_blocks;
    vol->sb.manifest_start  = manifest_start;
    vol->sb.manifest_blocks = manifest_blocks;
    vol->sb.manifest_count  = manifest_entries;
    vol->sb.data_start      = data_start;
    vol->sb.reserved_blocks = reserved_blocks;
    vol->sb.root_id         = 0;                /* entry 0 = root directory */
    vol->sb.feature_flags   = CXFS_FEAT_TIMESTAMPS | CXFS_FEAT_PERMS | CXFS_FEAT_LOCKING;
    vol->sb.entry_size      = CXFS_ENTRY_SIZE;
    if (format_label[0]) strlcpy(vol->sb.label, format_label, CXFS_LABEL_LEN);
    format_label[0] = '\0';     /* one format, one label - never a leftover */
    vol->sb.created         = 0;                /* timestamp wired in a later step */
    vol->sb.modified        = 0;

    /* Format is held to the same standard as mount, against the same function:
       a caller passing a total_blocks too small to hold the layout computed
       above would otherwise write a superblock whose data_start is past the end
       of the volume, and then mount it. Checked before anything is written, so
       a refused format leaves the disk alone. */
    if (cxfs_sb_validate(&vol->sb, vol->base_lba, vol_disk_sectors()) != 0) return -1;

    if (write_block(0, &vol->sb) != 0) return -1;

    /* --- zero the bitmap, then mark metadata blocks used --- */
    uint8_t *block = dat_blk;              /* staging; see block staging buffers */
    memset(block, 0, CXFS_BLOCK_SIZE);
    for (uint32_t i = 0; i < bitmap_blocks; i++)
        if (write_block(bitmap_start + i, block) != 0) return -1;

    /* mark blocks 0 .. data_start-1 as used (superblock+bitmap+manifest).
       we set bits in the first bitmap block(s). */
    uint32_t used_through = data_start;    /* all metadata blocks */
    memset(block, 0, CXFS_BLOCK_SIZE);
    for (uint32_t b = 0; b < used_through; b++) {
        uint32_t byte = b / 8;
        uint32_t bit  = b % 8;
        /* this simple loop assumes used_through fits in the first bitmap block
           (data_start is small: 1 + bitmap_blocks + manifest_blocks). */
        block[byte] |= (1u << bit);
    }
    if (write_block(bitmap_start, block) != 0) return -1;

    /* --- zero the manifest table --- */
    memset(block, 0, CXFS_BLOCK_SIZE);
    for (uint32_t i = 0; i < manifest_blocks; i++)
        if (write_block(manifest_start + i, block) != 0) return -1;

    /* --- create the root directory as entry 0 --- */
    struct cxfs_entry root;
    memset(&root, 0, sizeof(root));
    root.id        = 0;
    root.parent_id = 0;                    /* root is its own parent */
    root.type      = CXFS_TYPE_DIR;
    strlcpy(root.name, "/", CXFS_NAME_LEN);
    root.name_len  = 1;
    /* write entry 0 into the first manifest block */
    memset(block, 0, CXFS_BLOCK_SIZE);
    memcpy(block, &root, sizeof(root));
    if (write_block(manifest_start, block) != 0) return -1;

    vol->mounted = 1;
    bitmap_load();
    manifest_load();
    return 0;
}

int cxfs_format(void) {
    /* whole-disk dev default: base 0, fixed 16 MB volume */
    return cxfs_format_at(0, (16u * 1024 * 1024) / CXFS_BLOCK_SIZE);
}

int cxfs_mount_at(uint64_t base_lba) {
    if (!disk_present()) return -1;

    /* This volume starts at base_lba (a partition offset, or 0 for whole-disk).
       The superblock is the volume's block 0 = sector base_lba. Read that sector
       for the geometry, then the full superblock. The location we were given is
       authoritative; vol->sb.base_lba is advisory. */
    vol->base_lba = base_lba;
    uint8_t first[512];
    if (disk_read(vol->disk_id, vol->base_lba, 1, first) != 0) return -1;

    /* Pull the three fields out by offset rather than casting `first` to a
       struct cxfs_superblock *: the struct is a whole 4KB block and this buffer
       is one 512-byte sector, so the cast points past the end of the object.
       Only the leading fields are ever touched, but the compiler is right that
       the cast is a lie, and it would become a real one the day a field moves. */
    uint32_t probe_magic;
    uint16_t probe_version, probe_block_size;
    memcpy(&probe_magic,      first + 0, sizeof probe_magic);
    memcpy(&probe_version,    first + 4, sizeof probe_version);
    memcpy(&probe_block_size, first + 6, sizeof probe_block_size);
    if (probe_magic   != CXFS_MAGIC)   { vol->mounted = 0; return -1; }
    if (probe_version != CXFS_VERSION) { vol->mounted = 0; return -1; }  /* v2 only */

    /* The geometry is NOT taken from the disk. It used to be -
       `sectors_per_block = probe_block_size / 512` - and that is what made the
       overflow: the field is a uint16_t, so the quotient reached 127, and the
       very next line read that many sectors into vol->sb, which is 4096 bytes.
       disk_read's count is a uint32_t and disk_check bounds it against the
       DEVICE, never the destination, so up to 65024 bytes landed in a 4096-byte
       struct - off the end of volumes[] and through `uint8_t *manifest`, a
       pointer manifest_load then writes 4KB blocks through.
       This driver only implements 4KB blocks, so the constant is the truth and
       nothing a disk says can size a transfer. A volume whose superblock
       disagrees is rejected by cxfs_sb_validate below, which is a comparison
       rather than an arithmetic input. */
    vol->sectors_per_block = CXFS_BLOCK_SIZE / 512;
    if (probe_block_size != CXFS_BLOCK_SIZE) { vol->mounted = 0; return -1; }

    /* keep vol->base_lba = base_lba (where we actually found the volume) */
    if (read_block(0, &vol->sb) != 0) { vol->mounted = 0; return -1; }

    /* Everything the rest of this driver turns into a block address comes out
       of here, so it is all checked once, before anything uses it. */
    if (cxfs_sb_validate(&vol->sb, vol->base_lba, vol_disk_sectors()) != 0) {
        vol->mounted = 0;
        return -1;
    }

    vol->mounted = 1;
    bitmap_load();
    manifest_load();
    return 0;
}

int cxfs_format_labeled(uint64_t base_lba, uint32_t total_blocks, const char *label) {
    if (label && label[0]) strlcpy(format_label, label, CXFS_LABEL_LEN);
    else                   format_label[0] = '\0';
    return cxfs_format_at(base_lba, total_blocks);
}

int cxfs_mount(void) { return cxfs_mount_at(0); }

/* ---- additional volumes ---- */

static void label_default(char *dst, uint32_t v) {
    /* A volume whose superblock predates the label field, or was formatted
       without one, still needs a name to answer to. "Volume1", "Volume2", ... */
    const char *p = "Volume";
    int i = 0;
    while (*p) dst[i++] = *p++;
    dst[i++] = (char)('0' + (v % 10));
    dst[i]   = '\0';
}

int cxfs_mount_volume(uint8_t disk_id, uint64_t base_lba,
                      uint32_t parent_dir, const char *label) {
    if (!volumes[0].mounted) return -1;          /* nothing to mount onto yet */

    /* The parent must be a directory that exists, on a volume already mounted.
       Checking it BEFORE taking a slot means a bad one costs nothing. */
    struct cxfs_entry pd;
    if (cxfs_read_entry(parent_dir, &pd) != 0) return -1;
    if (pd.type != CXFS_TYPE_DIR) return -1;

    uint32_t slot = 0;
    for (uint32_t v = 1; v < CXFS_MAX_VOLUMES; v++)
        if (!volumes[v].mounted) { slot = v; break; }
    if (!slot) return -1;                        /* no free slot */

    struct cxfs_volume *prev = vol;
    vol = &volumes[slot];
    vol->disk_id  = disk_id;
    vol->manifest = NULL;                        /* only the root gets the cache */
    vol->manifest_blocks_cached = 0;
    vol->placed   = 0;
    int rc = cxfs_mount_at(base_lba);            /* fills sb, loads the bitmap */
    vol = prev;
    if (rc != 0) { volumes[slot].mounted = 0; return -1; }

    /* The label decides the mount point's name, and it is only knowable once
       the superblock is in - which is why the caller passes the PARENT and the
       directory is made here rather than by the caller beforehand. */
    if (label && label[0])                 strlcpy(volumes[slot].label, label, CXFS_LABEL_LEN);
    else if (volumes[slot].sb.label[0])    strlcpy(volumes[slot].label, volumes[slot].sb.label, CXFS_LABEL_LEN);
    else                                   label_default(volumes[slot].label, slot);

    /* Reuse the directory if it is already there - a volume unmounted and
       remounted should land back on the same path, not on "Data_1". */
    int mp = cxfs_find_in_dir(parent_dir, volumes[slot].label);
    if (mp < 0) mp = cxfs_create_entry(parent_dir, volumes[slot].label, CXFS_TYPE_DIR);
    if (mp < 0) { volumes[slot].mounted = 0; return -1; }

    /* Nothing else may already be mounted there. */
    for (uint32_t v = 1; v < CXFS_MAX_VOLUMES; v++)
        if (v != slot && volumes[v].mounted && volumes[v].placed &&
            volumes[v].mount_point == (uint32_t)mp) {
            volumes[slot].mounted = 0;
            return -1;
        }

    volumes[slot].mount_point = (uint32_t)mp;
    volumes[slot].placed      = 1;               /* reachable from here on */
    return (int)slot;
}

int cxfs_unmount_volume(uint32_t v) {
    if (v == 0 || v >= CXFS_MAX_VOLUMES) return -1;   /* never the root */
    if (!volumes[v].mounted) return -1;

    /* Every write goes straight through to the disk - the manifest cache is
       write-through and the bitmap is flushed per changed block - so there is
       nothing buffered to lose and unmounting is just forgetting. */
    volumes[v].mounted = 0;
    volumes[v].placed  = 0;
    volumes[v].manifest_blocks_cached = 0;
    volumes[v].bitmap_loaded = 0;
    volumes[v].label[0] = '\0';
    if (vol == &volumes[v]) vol = &volumes[0];
    return 0;
}

uint32_t cxfs_volume_slots(void) { return CXFS_MAX_VOLUMES; }

int cxfs_volume_mounted(uint32_t v) {
    return (v < CXFS_MAX_VOLUMES) ? volumes[v].mounted : 0;
}

const char *cxfs_volume_label(uint32_t v) {
    if (v >= CXFS_MAX_VOLUMES || !volumes[v].mounted) return 0;
    return volumes[v].label;
}

uint32_t cxfs_volume_mount_point(uint32_t v) {
    return (v < CXFS_MAX_VOLUMES && volumes[v].mounted) ? volumes[v].mount_point : 0;
}

/* "Is the filesystem up" is a question about the root volume. Answering it
   from `vol` would make it depend on which volume was touched last. */
int cxfs_is_mounted(void) { return volumes[0].mounted; }

const struct cxfs_superblock *cxfs_get_superblock(void) {
    return vol->mounted ? &vol->sb : 0;
}

/* ====================================================================
 * Block allocator (data blocks) - bitmap cached in RAM
 *
 * The on-disk bitmap is small (a few KB). We load it into a RAM buffer at
 * mount and do all test/set against memory (instant), writing only changed
 * bitmap blocks back to disk. This avoids a disk read per bit-check, which
 * was making allocation and free-counting take seconds.
 * ==================================================================== */


/* load the whole bitmap from disk into RAM */
static void bitmap_load(void) {
    vol->bitmap_bytes = (vol->sb.total_blocks + 7) / 8;
    if (vol->bitmap_bytes > CXFS_MAX_BITMAP_BYTES)
        vol->bitmap_bytes = CXFS_MAX_BITMAP_BYTES;
    for (uint32_t i = 0; i < vol->sb.bitmap_blocks; i++) {
        uint8_t *buf = bmp_blk;
        if (read_block(vol->sb.bitmap_start + i, buf) != 0) break;
        uint32_t off = i * CXFS_BLOCK_SIZE;
        for (uint32_t j = 0; j < CXFS_BLOCK_SIZE && off + j < vol->bitmap_bytes; j++)
            vol->bitmap[off + j] = buf[j];
    }
    vol->bitmap_loaded = 1;
}

/* write the bitmap block containing `block`'s bit back to disk */
static void bitmap_flush_for(uint32_t block) {
    uint32_t byte = block / 8;
    uint32_t which = byte / CXFS_BLOCK_SIZE;   /* which bitmap block */
    uint8_t *buf = bmp_blk;
    uint32_t off = which * CXFS_BLOCK_SIZE;
    for (uint32_t j = 0; j < CXFS_BLOCK_SIZE; j++)
        buf[j] = (off + j < vol->bitmap_bytes) ? vol->bitmap[off + j] : 0;
    write_block(vol->sb.bitmap_start + which, buf);
}

static int bitmap_test(uint32_t block) {
    uint32_t byte = block / 8, bit = block % 8;
    if (byte >= vol->bitmap_bytes) return 1;
    return (vol->bitmap[byte] >> bit) & 1;
}

static void bitmap_set(uint32_t block, int used) {
    uint32_t byte = block / 8, bit = block % 8;
    if (byte >= vol->bitmap_bytes) return;
    if (used) vol->bitmap[byte] |=  (1u << bit);
    else      vol->bitmap[byte] &= ~(1u << bit);
    bitmap_flush_for(block);   /* persist just the changed block */
}

/* ====================================================================
 * Manifest cache - the same trick as the bitmap, for the same reason
 *
 * A directory stores no data blocks: membership is derived by scanning the
 * WHOLE manifest for entries whose parent_id matches. The manifest is 64
 * blocks (1024 entries x 256B = 256KB), so one path component costs up to 64
 * block reads and cxfs_resolve does that per component - half a megabyte of
 * disk I/O to resolve "/System/Boot.xoex", a megabyte for a four-deep path.
 *
 * The allocation bitmap had exactly this shape and the comment above it
 * records what it cost: per-bit disk reads "was making allocation and
 * free-counting take seconds". Same fix here - hold the manifest in RAM, scan
 * memory, and write through to disk on every entry write so the volume is
 * never stale and there is nothing to flush at unmount.
 *
 * The cache is an ACCELERATOR, not a requirement. A volume whose manifest is
 * larger than the buffer (manifest_count is a superblock field, so a future
 * profile may exceed the standard 1024) simply stays uncached and every reader
 * falls back to the disk path it used before. manifest_block() returning NULL
 * is that fallback, and it is the only thing a caller has to handle.
 * ==================================================================== */


/* Load the whole manifest into RAM. Called at mount and after format. Leaves
   vol->manifest_blocks_cached at 0 on any failure, which is not an error - it means
   the readers take the disk path. */
static void manifest_load(void) {
    vol->manifest_blocks_cached = 0;
    if (!vol->manifest) return;                  /* this volume has no cache */
    if (!vol->sb.manifest_blocks) return;
    if ((uint64_t)vol->sb.manifest_blocks * CXFS_BLOCK_SIZE > CXFS_MAX_MANIFEST_BYTES) return;

    for (uint32_t i = 0; i < vol->sb.manifest_blocks; i++) {
        if (read_block(vol->sb.manifest_start + i,
                       vol->manifest + (size_t)i * CXFS_BLOCK_SIZE) != 0)
            return;                      /* partial load is no load */
    }
    vol->manifest_blocks_cached = vol->sb.manifest_blocks;
}

/* Cached manifest block `rel` (relative to manifest_start), or NULL if this
   volume is not cached. */
static uint8_t *manifest_block(uint32_t rel) {
    if (!vol->manifest || rel >= vol->manifest_blocks_cached) return NULL;
    return vol->manifest + (size_t)rel * CXFS_BLOCK_SIZE;
}

/* Fetch manifest block `rel` for reading: the cache when there is one, else
   staged through `stage` from disk. Returns NULL only on a disk error.
 *
 * The caller picks the staging buffer because the UNCACHED path still has the
 * aliasing problem the per-role buffers exist to prevent: cxfs_list_dir holds
 * a block across a callback, and if that callback reads an entry of its own
 * through the same buffer it rewrites the block being iterated. Cached, every
 * caller gets a distinct pointer into the cache and the question does not
 * arise - but the fallback has to stay correct on its own terms. */
static const uint8_t *manifest_fetch_into(uint32_t rel, uint8_t *stage) {
    const uint8_t *m = manifest_block(rel);
    if (m) return m;
    if (read_block(vol->sb.manifest_start + rel, stage) != 0) return NULL;
    return stage;
}

static const uint8_t *manifest_fetch(uint32_t rel) {
    return manifest_fetch_into(rel, ent_blk);
}

uint32_t cxfs_alloc_block(void) {
    if (!vol->mounted) return 0;
    uint32_t first = vol->sb.data_start + vol->sb.reserved_blocks;
    for (uint32_t b = first; b < vol->sb.total_blocks; b++) {
        if (!bitmap_test(b)) {       /* RAM check - instant */
            bitmap_set(b, 1);        /* one disk write */
            return b;
        }
    }
    return 0;
}

void cxfs_free_block(uint32_t block) {
    if (!vol->mounted) return;
    if (block < vol->sb.data_start) return;
    bitmap_set(block, 0);
}

/* --- batched allocation ----------------------------------------------------
 * bitmap_set flushes the bitmap block it touched on every single call, so
 * allocating N blocks through cxfs_alloc_block costs N 4KB disk writes just to
 * record the allocation. Growing a file by a megabyte would be 256 of them.
 * These mark bits in RAM and flush each affected bitmap block once.
 */
static void bitmap_mark(uint32_t block, int used) {
    uint32_t byte = block / 8, bit = block % 8;
    if (byte >= vol->bitmap_bytes) return;
    if (used) vol->bitmap[byte] |=  (1u << bit);
    else      vol->bitmap[byte] &= ~(1u << bit);
}

/* flush every bitmap block holding a bit for blocks [first, last]. */
static void bitmap_flush_span(uint32_t first, uint32_t last) {
    uint32_t w0 = (first / 8) / CXFS_BLOCK_SIZE;
    uint32_t w1 = (last  / 8) / CXFS_BLOCK_SIZE;
    /* w * CXFS_BLOCK_SIZE * 8 is the first block whose bit lives in bitmap
       block w, which is all bitmap_flush_for needs to find that block. */
    for (uint32_t w = w0; w <= w1; w++)
        bitmap_flush_for(w * CXFS_BLOCK_SIZE * 8);
}

uint32_t cxfs_alloc_run(uint32_t n) {
    if (!vol->mounted || n == 0) return 0;
    uint32_t first = vol->sb.data_start + vol->sb.reserved_blocks;

    /* first-fit over the RAM bitmap: walk looking for n free in a row. On a
       used block, restart the candidate run after it. */
    uint32_t start = first, have = 0;
    for (uint32_t b = first; b < vol->sb.total_blocks; b++) {
        if (bitmap_test(b)) { start = b + 1; have = 0; continue; }
        if (have == 0) start = b;
        if (++have == n) {
            for (uint32_t i = 0; i < n; i++) bitmap_mark(start + i, 1);
            bitmap_flush_span(start, start + n - 1);
            return start;
        }
    }
    return 0;   /* no run that long is free */
}

/* Is [start, start+len) a run of real data blocks on this volume?
 *
 * A manifest entry is untrusted for the same reason the superblock is - it came
 * off a disk someone else may have written - and no extent field was ever
 * checked against anything. An entry claiming extent_start in the metadata
 * region, or past the end of the volume, had its blocks read and written: the
 * address goes to read_block, which adds base_lba and asks the device, so a
 * crafted extent reaches other partitions.
 *
 * len == 0 is a vacuously valid empty extent, which is what an unused slot is.
 * The length test is written as a subtraction because start + len wraps.
 */
static int extent_ok(uint32_t start, uint32_t len) {
    if (len == 0)                       return 1;
    if (!vol->mounted)                  return 0;
    if (start <  vol->sb.data_start)    return 0;   /* the metadata region */
    if (start >= vol->sb.total_blocks)  return 0;
    if (len   >  vol->sb.total_blocks - start) return 0;
    return 1;
}

/* Every extent of an entry is a real run on this volume.
 *
 * Checked once at each I/O entry point rather than per block, so a file whose
 * manifest entry is corrupt is one honest refusal instead of a partial read of
 * whatever those addresses happen to land on. It also makes extent_blocks()
 * safe by precondition: eight validated lengths sum well inside a uint32_t,
 * where eight arbitrary ones wrap. */
static int entry_extents_ok(const struct cxfs_entry *e) {
    for (int i = 0; i < CXFS_MAX_EXTENTS; i++)
        if (!extent_ok(e->extent_start[i], e->extent_len[i])) return 0;
    return 1;
}

/* release `n` consecutive blocks starting at `first`, one flush per bitmap
   block rather than per released block. */
static void cxfs_free_run(uint32_t first, uint32_t n) {
    if (!vol->mounted || n == 0) return;
    if (first < vol->sb.data_start) return;
    /* An out-of-range run is refused rather than clamped, and this is the
       destructive one of the extent findings (§3). bitmap_mark is bounded by
       bitmap_bytes, so the MARKS were always safe - but bitmap_flush_span then
       looped over every bitmap block the span touched and wrote a zero-filled
       block to bitmap_start + w for each. With a corrupt extent_len of, say,
       0x10000000, that walks `w` far past bitmap_blocks and zeroes the manifest
       and the data region: one cxfs_delete_entry destroyed the volume. */
    if (!extent_ok(first, n)) return;
    for (uint32_t i = 0; i < n; i++) bitmap_mark(first + i, 0);
    bitmap_flush_span(first, first + n - 1);
}

uint32_t cxfs_free_blocks(void) {
    /* The root volume's free space, for the same reason cxfs_is_mounted
       answers for the root: its one caller is the boot log, which means "/". */
    struct cxfs_volume *prev = vol;
    vol = &volumes[0];
    uint32_t count = 0;
    if (vol->mounted)
        for (uint32_t b = vol->sb.data_start; b < vol->sb.total_blocks; b++)
            if (!bitmap_test(b)) count++;   /* RAM scan - fast */
    vol = prev;
    return count;
}

/* ====================================================================
 * Manifest / entry operations
 * ==================================================================== */

int cxfs_read_entry(uint32_t id, struct cxfs_entry *out) {
    if (!vol_select(id)) return -1;
    uint32_t local = IDX_OF(id);
    if (local >= vol->sb.manifest_count) return -1;
    uint32_t rel = local / ENTRIES_PER_BLOCK;
    uint32_t idx = local % ENTRIES_PER_BLOCK;

    const uint8_t *buf = manifest_fetch(rel);
    if (!buf) return -1;
    memcpy(out, buf + idx * sizeof(struct cxfs_entry), sizeof(struct cxfs_entry));

    /* A name is 64 bytes on disk and nothing guarantees one of them is a zero.
       strlcpy measures its source before consulting `size`, so an unterminated
       name made cxfs_path_of read past the name field, through the rest of the
       256-byte entry and potentially past the object - a path built out of
       whatever followed. One byte, at the one place every entry comes through. */
    out->name[CXFS_NAME_LEN - 1] = '\0';

    /* On the way out, the two id fields become the caller's kind of id. Doing
       it here means every caller that hands e.id or e.parent_id straight back
       to this driver keeps working without knowing volumes exist. */
    uint32_t v = VOL_OF(id);
    out->id        = TAG_ID(v, out->id);
    out->parent_id = TAG_ID(v, out->parent_id);
    return 0;
}

int cxfs_write_entry(const struct cxfs_entry *entry) {
    if (!vol_select(entry->id)) return -1;
    uint32_t local = IDX_OF(entry->id);
    if (local >= vol->sb.manifest_count) return -1;
    uint32_t rel = local / ENTRIES_PER_BLOCK;
    uint32_t idx = local % ENTRIES_PER_BLOCK;

    /* The mirror of the tagging in cxfs_read_entry: what goes on the disk is
       volume-local, so both id fields lose their tag on the way down. The
       parent must be on this volume too - a parent pointer cannot reach across
       a filesystem boundary, and silently truncating one would graft the entry
       onto whatever slot that index happens to hold here. */
    if (VOL_OF(entry->parent_id) != VOL_OF(entry->id)) return -1;

    struct cxfs_entry on_disk = *entry;
    on_disk.id        = local;
    on_disk.parent_id = IDX_OF(entry->parent_id);

    /* Write THROUGH: update the cache and the disk together, so a reader never
       sees one without the other and there is nothing to flush at unmount. */
    uint8_t *buf = manifest_block(rel);
    if (!buf) {
        buf = ent_blk;                            /* uncached: read-modify-write */
        if (read_block(vol->sb.manifest_start + rel, buf) != 0) return -1;
    }
    memcpy(buf + idx * sizeof(struct cxfs_entry), &on_disk, sizeof(struct cxfs_entry));
    return write_block(vol->sb.manifest_start + rel, buf);
}

int cxfs_alloc_entry(void) {
    if (!vol->mounted) return -1;
    /* Scan a whole block of entries at a time. With the manifest cached this is
       a RAM scan; uncached it is one disk read per block, as it always was. */
    for (uint32_t blk = 0; blk < vol->sb.manifest_blocks; blk++) {
        const uint8_t *buf = manifest_fetch(blk);
        if (!buf) return -1;
        for (uint32_t i = 0; i < ENTRIES_PER_BLOCK; i++) {
            uint32_t id = blk * ENTRIES_PER_BLOCK + i;
            if (id == 0) continue;                 /* root */
            if (id >= vol->sb.manifest_count) return -1;
            struct cxfs_entry *e = (struct cxfs_entry *)(buf + i * sizeof(struct cxfs_entry));
            if (e->type == CXFS_TYPE_FREE)
                return (int)TAG_ID(vol - volumes, id);
        }
    }
    return -1;
}

/* case-insensitive ASCII compare of two names */
static int name_equals(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;   /* both ended together */
}

int cxfs_find_in_dir(uint32_t parent_id, const char *name) {
    if (!vol_select(parent_id)) return -1;
    /* The manifest holds volume-local indices, so the comparison is against
       the untagged parent and the answer is tagged on the way out. */
    uint32_t v      = VOL_OF(parent_id);
    uint32_t parent = IDX_OF(parent_id);
    for (uint32_t blk = 0; blk < vol->sb.manifest_blocks; blk++) {
        const uint8_t *buf = manifest_fetch(blk);
        if (!buf) continue;
        for (uint32_t i = 0; i < ENTRIES_PER_BLOCK; i++) {
            uint32_t id = blk * ENTRIES_PER_BLOCK + i;
            if (id >= vol->sb.manifest_count) return -1;
            struct cxfs_entry *e = (struct cxfs_entry *)(buf + i * sizeof(struct cxfs_entry));
            if (e->type == CXFS_TYPE_FREE) continue;
            if (e->parent_id == parent && id != parent &&
                name_equals(e->name, name))
                return (int)TAG_ID(v, id);
        }
    }
    return -1;
}

int cxfs_normalize_name(char *name) {
    if (!name || name[0] == '\0') return -1;
    /* must fit in the on-disk name field WITH room for a null terminator */
    size_t len = 0;
    while (name[len]) len++;
    if (len >= CXFS_NAME_LEN) return -1;     /* too long - reject */
    for (char *p = name; *p; p++) {
        if (*p == ' ') *p = '_';                 /* spaces -> underscore */
        else if (*p == '/') return -1;           /* path separator illegal */
        else if ((unsigned char)*p < 0x20) return -1;  /* control chars illegal */
    }
    return 0;
}

/* ====================================================================
 * Directory operations + path resolution
 * ==================================================================== */

int cxfs_create_entry(uint32_t parent_id, const char *name, uint8_t type) {
    /* The new entry is allocated on the PARENT's volume - selecting it here is
       what makes the cxfs_alloc_entry below take a slot from the right one. */
    if (!vol_select(parent_id)) return -1;

    /* Copy + normalize the name. The copy REFUSES a name that does not fit
       rather than cutting it down: this loop used to truncate into nm and then
       hand the short version to cxfs_normalize_name, whose `len >=
       CXFS_NAME_LEN` rejection therefore could never fire from here. A 100
       character name silently became a 63 character file, and two different
       long names became the same one - at which point the "must not already
       exist" check below refused a collision the caller had not caused. See
       the bounded-string contract in string.h, rule 4. */
    if (!name) return -1;
    char nm[CXFS_NAME_LEN];
    if (strlcpy(nm, name, sizeof nm) >= sizeof nm) return -1;   /* too long */
    if (cxfs_normalize_name(nm) != 0) return -1;                /* bad name */
    int i = 0;
    while (nm[i]) i++;                               /* name_len, post-normalize */

    /* must not already exist in this directory */
    if (cxfs_find_in_dir(parent_id, nm) >= 0) return -1;

    /* parent must be a directory */
    struct cxfs_entry parent;
    if (cxfs_read_entry(parent_id, &parent) != 0) return -1;
    if (parent.type != CXFS_TYPE_DIR) return -1;

    int id = cxfs_alloc_entry();
    if (id < 0) return -1;                            /* manifest full */

    struct cxfs_entry e;
    memset(&e, 0, sizeof(e));
    e.id        = (uint32_t)id;
    e.parent_id = parent_id;
    e.type      = type;
    strlcpy(e.name, nm, CXFS_NAME_LEN);
    e.name_len  = (uint8_t)i;
    e.size      = 0;
    e.created = e.modified = e.accessed = cxfs_now();   /* v2 timestamps */
    e.owner_uid = current_uid();                        /* v2: creator owns it */
    e.group_id  = 0;
    e.permissions = (type == CXFS_TYPE_DIR) ? CXFS_PERM_DIR_DEFAULT
                                            : CXFS_PERM_FILE_DEFAULT;
    if (cxfs_write_entry(&e) != 0) return -1;
    return id;
}

int cxfs_resolve(const char *path, uint32_t cwd) {
    if (!path) return -1;

    /* An absolute path always starts at the ROOT volume, never at whichever
       volume happened to be selected last. A relative one starts at the
       working directory, and the tag on that id says which volume that is. */
    uint32_t current;
    if (path[0] == '/') {
        if (!volumes[0].mounted) return -1;
        current = cross_mount(TAG_ID(0, volumes[0].sb.root_id));
    } else {
        if (!vol_select(cwd)) return -1;
        current = cwd;
    }

    /* walk components separated by '/' */
    const char *p = path;
    while (*p == '/') p++;                 /* skip leading slashes */

    char comp[CXFS_NAME_LEN];
    while (*p) {
        int n = 0;
        while (*p && *p != '/' && n < CXFS_NAME_LEN - 1) comp[n++] = *p++;
        comp[n] = '\0';
        while (*p == '/') p++;             /* skip slashes to next component */

        if (n == 0) continue;
        if (comp[0] == '.' && comp[1] == '\0') continue;          /* "." */
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {/* ".." */
            /* Step out of a mounted volume first: its root's parent is itself,
               so ".." would otherwise stop dead at the mount point instead of
               going back to the directory above it. */
            current = uncross_mount(current);
            struct cxfs_entry e;
            if (cxfs_read_entry(current, &e) != 0) return -1;
            current = e.parent_id;        /* root's parent is itself */
            continue;
        }

        int found = cxfs_find_in_dir(current, comp);
        if (found < 0) return -1;         /* component doesn't exist */
        /* If that directory is a mount point, the path continues on the volume
           mounted there rather than inside the (empty) directory itself. */
        current = cross_mount((uint32_t)found);
    }
    return (int)current;
}

void cxfs_list_dir(uint32_t parent_id, void (*cb)(const struct cxfs_entry *)) {
    if (!cb || !vol_select(parent_id)) return;
    uint32_t v      = VOL_OF(parent_id);
    uint32_t parent = IDX_OF(parent_id);
    for (uint32_t blk = 0; blk < vol->sb.manifest_blocks; blk++) {
        const uint8_t *buf = manifest_fetch_into(blk, dir_blk);
        if (!buf) continue;
        for (uint32_t i = 0; i < ENTRIES_PER_BLOCK; i++) {
            uint32_t id = blk * ENTRIES_PER_BLOCK + i;
            if (id >= vol->sb.manifest_count) return;
            if (id == 0) continue;        /* skip root itself */
            struct cxfs_entry *e = (struct cxfs_entry *)(buf + i * sizeof(struct cxfs_entry));
            if (e->type == CXFS_TYPE_FREE) continue;
            if (e->parent_id != parent) continue;
            /* The callback gets a tagged COPY. Tagging `e` in place would be
               writing into the manifest cache, which is the volume's real
               contents, not a scratch buffer. */
            struct cxfs_entry out = *e;
            out.id        = TAG_ID(v, out.id);
            out.parent_id = TAG_ID(v, out.parent_id);
            cb(&out);
        }
    }
}

void cxfs_path_of(uint32_t id, char *out, int cap) {
    if (cap <= 0) return;
    if (!vol_select(id)) { out[0] = '\0'; return; }

    /* The one path that is just "/" is the ROOT volume's root. The root of a
       mounted volume has a real path - the mount point's - and the walk below
       produces it. */
    if (id == TAG_ID(0, volumes[0].sb.root_id)) {
        if (cap > 1) { out[0] = '/'; out[1] = '\0'; } else out[0] = '\0';
        return;
    }

    /* collect names from `id` up to the root volume's root, then emit reversed */
    char names[16][CXFS_NAME_LEN];   /* up to 16 levels deep */
    int depth = 0;
    uint32_t cur = id;
    while (depth < 16) {
        /* At the root of a mounted volume, carry on from the directory it is
           mounted on - that directory's NAME is the next component, so the
           path reads as one path across the boundary. */
        uint32_t out_of = uncross_mount(cur);
        if (out_of != cur) { cur = out_of; continue; }
        if (cur == TAG_ID(0, volumes[0].sb.root_id)) break;

        struct cxfs_entry e;
        if (cxfs_read_entry(cur, &e) != 0) break;
        strlcpy(names[depth], e.name, CXFS_NAME_LEN);
        depth++;
        if (e.parent_id == cur) break;   /* safety */
        cur = e.parent_id;
    }

    int pos = 0;
    for (int d = depth - 1; d >= 0; d--) {
        if (pos < cap - 1) out[pos++] = '/';
        for (int k = 0; names[d][k] && pos < cap - 1; k++) out[pos++] = names[d][k];
    }
    out[pos < cap ? pos : cap - 1] = '\0';
}

/* ====================================================================
 * File content - extent-based storage
 * ==================================================================== */

/* free every block an entry's extents reference and clear the extents. Leaves
   e->size alone - the callers that want it zeroed do that themselves. */
static void free_extents(struct cxfs_entry *e) {
    for (int i = 0; i < CXFS_MAX_EXTENTS; i++) {
        /* a run at a time: cxfs_free_block would flush the bitmap block once
           per released block, which for a large file is hundreds of 4KB writes */
        if (e->extent_len[i]) cxfs_free_run(e->extent_start[i], e->extent_len[i]);
        e->extent_start[i] = 0;
        e->extent_len[i]   = 0;
    }
}

/* free every data block referenced by an entry's extents, clear them. */
void cxfs_free_file_data(struct cxfs_entry *e) {
    free_extents(e);
    e->size = 0;
}

/* Whole-file write: cut the file to nothing, then lay `len` bytes down at 0.
 *
 * Expressed on top of the offset layer so there is exactly ONE place that grows
 * extents and one allocator path to get wrong. It also means a whole-file write
 * now compacts a file that has run out of extents instead of failing it, which
 * is what the old open-coded loop did ("too fragmented / too big for v1").
 *
 * Keeps its original contract: 0 on success, negative on failure - callers
 * (install.c, syslog.c, ktest.c) test `!= 0`, not a byte count.
 */
int cxfs_write_file(uint32_t id, const void *data, uint32_t len) {
    int rc = cxfs_truncate(id, 0);
    if (rc != CXFS_E_OK) return rc;
    if (len == 0) return 0;

    rc = cxfs_write_at(id, 0, data, len);
    if (rc < 0) return rc;
    return ((uint32_t)rc == len) ? 0 : CXFS_E_FAIL;   /* a short write is a failure here */
}

int cxfs_read_file(uint32_t id, void *buf, uint32_t cap) {
    if (!vol_select(id)) return -1;

    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (e.type != CXFS_TYPE_FILE) return -1;

    if (!cxfs_check_perm(&e, CXFS_ACC_READ)) return -1;    /* v2: permission */
    if (!entry_extents_ok(&e)) return -1;   /* corrupt extents: refuse, don't read */

    uint32_t want = e.size;
    if (want > cap) want = cap;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t got = 0;

    for (int i = 0; i < CXFS_MAX_EXTENTS && got < want; i++) {
        for (uint32_t b = 0; b < e.extent_len[i] && got < want; b++) {
            uint8_t *block = dat_blk;
            if (read_block(e.extent_start[i] + b, block) != 0) return -1;
            uint32_t chunk = want - got;
            if (chunk > CXFS_BLOCK_SIZE) chunk = CXFS_BLOCK_SIZE;
            for (uint32_t j = 0; j < chunk; j++) dst[got + j] = block[j];
            got += chunk;
        }
    }
    return (int)got;
}

/* Path-based convenience for loaders: resolve an absolute path from root, then
   stat / read it. cxfs_stat_path fills *out (use .size to size a buffer, .type
   to check it's a file); cxfs_read_path reads up to `cap` bytes and returns the
   bytes read (the file's size), or negative on error. */
int cxfs_stat_path(const char *path, struct cxfs_entry *out) {
    int id = cxfs_resolve(path, 0);          /* 0 = root; absolute paths */
    if (id < 0) return -1;
    return cxfs_read_entry((uint32_t)id, out);
}

int cxfs_read_path(const char *path, void *buf, uint32_t cap) {
    int id = cxfs_resolve(path, 0);
    if (id < 0) return -1;
    return cxfs_read_file((uint32_t)id, buf, cap);
}


/* ====================================================================
 * Offset-based file I/O (v3) - see the header for why this exists.
 * ==================================================================== */

/* how many blocks an entry's extents currently cover */
static uint32_t extent_blocks(const struct cxfs_entry *e) {
    uint32_t n = 0;
    for (int i = 0; i < CXFS_MAX_EXTENTS; i++) n += e->extent_len[i];
    return n;
}

/* index of the last extent in use, or -1 when the file owns nothing yet */
static int last_extent(const struct cxfs_entry *e) {
    int li = -1;
    for (int i = 0; i < CXFS_MAX_EXTENTS; i++) if (e->extent_len[i]) li = i;
    return li;
}

/* absolute block holding block index `bi` of the file, or 0 if `bi` is past
   what the extents cover. (0 is never a data block - it's the superblock - so
   it doubles as the failure value.) */
static uint32_t block_at(const struct cxfs_entry *e, uint32_t bi) {
    uint32_t seen = 0;
    for (int i = 0; i < CXFS_MAX_EXTENTS; i++) {
        if (bi < seen + e->extent_len[i]) {
            /* The I/O entry points already gated the whole entry through
               entry_extents_ok, so this is redundant - and it stays, for the
               reason ip_parse keeps its `ihl > avail` test: it makes the
               safety of the address this function hands to read_block LOCAL,
               rather than derived from a check in a caller. Deriving it is
               what left every extent unvalidated in the first place. */
            if (!extent_ok(e->extent_start[i], e->extent_len[i])) return 0;
            return e->extent_start[i] + (bi - seen);
        }
        seen += e->extent_len[i];
    }
    return 0;
}

/* write `n` zeroed blocks starting at `first`.
 *
 * Freshly allocated blocks hold whatever the previous owner left there, so
 * every block handed to a file gets zeroed before the file can read it. Two
 * reasons, and the second is the important one: a hole (a write past EOF) must
 * read back as zeros, and without this a process could allocate a block and
 * read another user's deleted file out of it. That is a capability leak, so
 * this is not an optimization to skip.
 *
 * It does cost a write per block that a full-block overwrite then repeats. The
 * seam to close that is to have the caller tell reserve_blocks which of the new
 * blocks it is about to overwrite whole; not worth the bookkeeping yet.
 */
static int zero_blocks(uint32_t first, uint32_t n) {
    memset(dat_blk, 0, CXFS_BLOCK_SIZE);
    for (uint32_t i = 0; i < n; i++)
        if (write_block(first + i, dat_blk) != 0) return CXFS_E_FAIL;
    return CXFS_E_OK;
}

/* Collapse a file into ONE extent of `total` blocks.
 *
 * This is the escape hatch for extent exhaustion. CXFS_MAX_EXTENTS is 8, and
 * the old write path simply failed the whole write when a file needed a ninth
 * ("too fragmented / too big for v1"). Instead: find one contiguous run big
 * enough for the whole file, copy the data across, release the old blocks, and
 * the file is back to a single extent with room to grow again.
 *
 * Copy first, free second: a failure part-way leaves the original blocks still
 * owned by the file, so the file is intact and only the new run leaks (and that
 * is released on the error path).
 */
static int compact_into_run(struct cxfs_entry *e, uint32_t total) {
    uint32_t have = extent_blocks(e);
    if (total < have) return CXFS_E_INVAL;

    uint32_t run = cxfs_alloc_run(total);
    if (!run) return CXFS_E_FRAGMENT;

    uint32_t dst = 0;
    for (int i = 0; i < CXFS_MAX_EXTENTS; i++) {
        if (!extent_ok(e->extent_start[i], e->extent_len[i])) {
            cxfs_free_run(run, total);
            return CXFS_E_FAIL;
        }
        for (uint32_t b = 0; b < e->extent_len[i]; b++) {
            if (read_block(e->extent_start[i] + b, dat_blk) != 0 ||
                write_block(run + dst, dat_blk) != 0) {
                cxfs_free_run(run, total);
                return CXFS_E_FAIL;
            }
            dst++;
        }
    }
    if (zero_blocks(run + have, total - have) != CXFS_E_OK) {
        cxfs_free_run(run, total);
        return CXFS_E_FAIL;
    }

    free_extents(e);                  /* the old blocks are now redundant */
    e->extent_start[0] = run;
    e->extent_len[0]   = total;
    return CXFS_E_OK;
}

/* Make sure the entry's extents cover at least `need` blocks. New blocks are
   zeroed. Does not touch e->size and does not write the entry back. */
static int reserve_blocks(struct cxfs_entry *e, uint32_t need) {
    uint32_t have = extent_blocks(e);
    if (have >= need) return CXFS_E_OK;
    uint32_t want = need - have;

    /* Preferred: one contiguous run for the whole shortfall. An append-heavy
       file stays at one or two extents this way, which is what keeps it from
       ever reaching the compaction path above. */
    uint32_t run = cxfs_alloc_run(want);
    if (run) {
        int li = last_extent(e);
        if (li >= 0 && e->extent_start[li] + e->extent_len[li] == run) {
            /* the run happens to abut the last extent: just extend it */
            if (zero_blocks(run, want) != CXFS_E_OK) { cxfs_free_run(run, want); return CXFS_E_FAIL; }
            e->extent_len[li] += want;
            return CXFS_E_OK;
        }
        if (li + 1 < CXFS_MAX_EXTENTS) {
            if (zero_blocks(run, want) != CXFS_E_OK) { cxfs_free_run(run, want); return CXFS_E_FAIL; }
            e->extent_start[li + 1] = run;
            e->extent_len[li + 1]   = want;
            return CXFS_E_OK;
        }
        /* no extent slot left to describe it - hand it back and compact */
        cxfs_free_run(run, want);
        return compact_into_run(e, need);
    }

    /* No run that long is free. Take what is there block by block, coalescing
       neighbours, and fall back to compaction if the extents run out. */
    while (have < need) {
        uint32_t blk = cxfs_alloc_block();
        if (!blk) return CXFS_E_NOSPACE;
        if (zero_blocks(blk, 1) != CXFS_E_OK) { cxfs_free_block(blk); return CXFS_E_FAIL; }

        int li = last_extent(e);
        if (li >= 0 && e->extent_start[li] + e->extent_len[li] == blk) {
            e->extent_len[li]++;
        } else if (li + 1 < CXFS_MAX_EXTENTS) {
            e->extent_start[li + 1] = blk;
            e->extent_len[li + 1]   = 1;
        } else {
            /* extents exhausted mid-grow: give this block back so the run
               compaction is looking for is not fragmented by it. */
            cxfs_free_block(blk);
            return compact_into_run(e, need);
        }
        have++;
    }
    return CXFS_E_OK;
}

/* Release whole blocks past block index `keep`, trimming the extents. */
static void trim_to_blocks(struct cxfs_entry *e, uint32_t keep) {
    uint32_t seen = 0;
    for (int i = 0; i < CXFS_MAX_EXTENTS; i++) {
        uint32_t len = e->extent_len[i];
        if (!len) continue;
        if (seen >= keep) {                       /* entirely past the cut */
            cxfs_free_run(e->extent_start[i], len);
            e->extent_start[i] = 0;
            e->extent_len[i]   = 0;
        } else if (seen + len > keep) {           /* straddles the cut */
            uint32_t k = keep - seen;
            cxfs_free_run(e->extent_start[i] + k, len - k);
            e->extent_len[i] = k;
        }
        seen += len;
    }
}

/* shared preamble: fetch the entry, confirm it is a writable file nobody else
   has locked. Returns 0, or a CXFS_E_* code. */
static int open_for_write(uint32_t id, struct cxfs_entry *e) {
    if (!vol_select(id)) return CXFS_E_FAIL;
    if (cxfs_read_entry(id, e) != 0)        return CXFS_E_NOTFOUND;
    if (e->type == CXFS_TYPE_DIR)           return CXFS_E_ISDIR;
    if (e->type != CXFS_TYPE_FILE)          return CXFS_E_INVAL;
    if (!cxfs_check_perm(e, CXFS_ACC_WRITE)) return CXFS_E_PERM;
    if (cxfs_lock_blocks(e))                return CXFS_E_LOCKED;
    if (!entry_extents_ok(e))               return CXFS_E_FAIL;
    return CXFS_E_OK;
}

int cxfs_read_at(uint32_t id, uint64_t off, void *buf, uint32_t len) {
    if (!vol_select(id)) return CXFS_E_FAIL;
    if (!buf)     return CXFS_E_INVAL;

    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0)      return CXFS_E_NOTFOUND;
    if (e.type == CXFS_TYPE_DIR)           return CXFS_E_ISDIR;
    if (e.type != CXFS_TYPE_FILE)          return CXFS_E_INVAL;
    if (!cxfs_check_perm(&e, CXFS_ACC_READ)) return CXFS_E_PERM;
    if (!entry_extents_ok(&e))             return CXFS_E_FAIL;

    if (off >= e.size) return 0;                      /* at or past EOF */
    uint64_t avail = e.size - off;
    if (len > avail) len = (uint32_t)avail;           /* short read at EOF */

    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < len) {
        uint32_t bi    = (uint32_t)((off + done) / CXFS_BLOCK_SIZE);
        uint32_t boff  = (uint32_t)((off + done) % CXFS_BLOCK_SIZE);
        uint32_t chunk = CXFS_BLOCK_SIZE - boff;
        if (chunk > len - done) chunk = len - done;

        uint32_t abs = block_at(&e, bi);
        if (!abs) return CXFS_E_FAIL;    /* size claims data the extents lack */

        if (chunk == CXFS_BLOCK_SIZE) {
            /* whole block: land it straight in the caller's buffer, no staging */
            if (read_block(abs, dst + done) != 0) return CXFS_E_FAIL;
        } else {
            if (read_block(abs, dat_blk) != 0) return CXFS_E_FAIL;
            memcpy(dst + done, dat_blk + boff, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

int cxfs_write_at(uint32_t id, uint64_t off, const void *data, uint32_t len) {
    if (!vol_select(id)) return CXFS_E_FAIL;
    if (!data)    return CXFS_E_INVAL;
    if (len == 0) return 0;

    struct cxfs_entry e;
    int rc = open_for_write(id, &e);
    if (rc != CXFS_E_OK) return rc;

    uint64_t end = off + len;
    if (end < off) return CXFS_E_INVAL;                       /* 64-bit wrap */
    uint64_t need64 = (end + CXFS_BLOCK_SIZE - 1) / CXFS_BLOCK_SIZE;
    if (need64 > 0xFFFFFFFFull) return CXFS_E_INVAL;

    /* Allocate everything up front, before any data is staged: reserve_blocks
       uses dat_blk for zeroing, so it must be done with it before the copy
       loop below starts using it. */
    rc = reserve_blocks(&e, (uint32_t)need64);
    if (rc != CXFS_E_OK) return rc;

    const uint8_t *src = (const uint8_t *)data;
    uint32_t done = 0;
    while (done < len) {
        uint32_t bi    = (uint32_t)((off + done) / CXFS_BLOCK_SIZE);
        uint32_t boff  = (uint32_t)((off + done) % CXFS_BLOCK_SIZE);
        uint32_t chunk = CXFS_BLOCK_SIZE - boff;
        if (chunk > len - done) chunk = len - done;

        uint32_t abs = block_at(&e, bi);
        if (!abs) return CXFS_E_FAIL;

        if (chunk == CXFS_BLOCK_SIZE) {
            /* whole block: straight from the caller's buffer */
            if (write_block(abs, src + done) != 0) return CXFS_E_FAIL;
        } else {
            /* partial block: read-modify-write, so the bytes either side of
               the written span survive */
            if (read_block(abs, dat_blk) != 0) return CXFS_E_FAIL;
            memcpy(dat_blk + boff, src + done, chunk);
            if (write_block(abs, dat_blk) != 0) return CXFS_E_FAIL;
        }
        done += chunk;
    }

    if (end > e.size) e.size = end;
    e.modified = e.accessed = cxfs_now();
    if (cxfs_write_entry(&e) != 0) return CXFS_E_FAIL;
    return (int)done;
}

int cxfs_truncate(uint32_t id, uint64_t new_size) {
    struct cxfs_entry e;
    int rc = open_for_write(id, &e);
    if (rc != CXFS_E_OK) return rc;
    if (new_size == e.size) return CXFS_E_OK;

    uint64_t need64 = (new_size + CXFS_BLOCK_SIZE - 1) / CXFS_BLOCK_SIZE;
    if (need64 > 0xFFFFFFFFull) return CXFS_E_INVAL;
    uint32_t keep = (uint32_t)need64;

    if (new_size < e.size) {
        /* Clear from the new end to the end of its block BEFORE releasing
           anything, so growing the file again later reads zeros there rather
           than the bytes that used to follow. */
        uint32_t tail = (uint32_t)(new_size % CXFS_BLOCK_SIZE);
        if (tail) {
            uint32_t abs = block_at(&e, keep - 1);
            if (abs) {
                if (read_block(abs, dat_blk) != 0) return CXFS_E_FAIL;
                memset(dat_blk + tail, 0, CXFS_BLOCK_SIZE - tail);
                if (write_block(abs, dat_blk) != 0) return CXFS_E_FAIL;
            }
        }
        trim_to_blocks(&e, keep);
    } else {
        /* Growing. The bytes between the old size and the end of its block are
           stale and now inside the file, so zero them; reserve_blocks zeroes
           whole new blocks for us. */
        uint32_t tail = (uint32_t)(e.size % CXFS_BLOCK_SIZE);
        if (tail) {
            uint32_t abs = block_at(&e, (uint32_t)(e.size / CXFS_BLOCK_SIZE));
            if (abs) {
                if (read_block(abs, dat_blk) != 0) return CXFS_E_FAIL;
                memset(dat_blk + tail, 0, CXFS_BLOCK_SIZE - tail);
                if (write_block(abs, dat_blk) != 0) return CXFS_E_FAIL;
            }
        }
        rc = reserve_blocks(&e, keep);
        if (rc != CXFS_E_OK) return rc;
    }

    e.size     = new_size;
    e.modified = e.accessed = cxfs_now();
    return (cxfs_write_entry(&e) == 0) ? CXFS_E_OK : CXFS_E_FAIL;
}

/* ====================================================================
 * Rename / move / delete
 * ==================================================================== */

int cxfs_rename(uint32_t id, const char *newname) {
    if (!vol_select(id)) return -1;
    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (id == TAG_ID(VOL_OF(id), vol->sb.root_id)) return -1;   /* don't rename root */

    /* Refuses rather than truncates, for the reason in cxfs_create_entry:
       renaming to an over-long name used to succeed and quietly give the file
       a different name than the one asked for. */
    if (!newname) return -1;
    char nm[CXFS_NAME_LEN];
    if (strlcpy(nm, newname, sizeof nm) >= sizeof nm) return -1;   /* too long */
    if (cxfs_normalize_name(nm) != 0) return -1;

    /* name_len is on-disk metadata the DevKit's CXFSEntry reads, and a rename
       used to leave it describing the old name. */
    e.name_len = (uint8_t)strlcpy(e.name, nm, CXFS_NAME_LEN);
    return cxfs_write_entry(&e);
}

int cxfs_is_ancestor(uint32_t ancestor, uint32_t id) {
    /* Nothing on one volume is an ancestor of anything on another: the walk
       below stops at a volume root, so it could never reach across anyway, and
       saying so here is clearer than letting it fall out. */
    if (VOL_OF(ancestor) != VOL_OF(id)) return 0;
    if (!vol_select(id)) return 0;
    uint32_t root = TAG_ID(VOL_OF(id), vol->sb.root_id);

    /* walk up from id to root; if we meet `ancestor`, it's an ancestor. */
    uint32_t cur = id;
    for (int guard = 0; guard < 1024; guard++) {
        if (cur == ancestor) return 1;
        if (cur == root) return 0;
        struct cxfs_entry e;
        if (cxfs_read_entry(cur, &e) != 0) return 0;
        if (e.parent_id == cur) return 0;   /* root safety */
        cur = e.parent_id;
    }
    return 0;
}

int cxfs_move(uint32_t id, uint32_t new_parent) {
    if (!vol_select(id)) return -1;
    if (id == TAG_ID(VOL_OF(id), vol->sb.root_id)) return -1;   /* can't move root */

    /* A move is one field change, and a parent pointer cannot span volumes.
       Moving a file to another filesystem means copying its blocks there and
       deleting the original, which is a caller's job, not this one's. */
    if (VOL_OF(new_parent) != VOL_OF(id)) return -1;

    struct cxfs_entry e, p;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (cxfs_read_entry(new_parent, &p) != 0) return -1;
    if (p.type != CXFS_TYPE_DIR) return -1; /* destination must be a dir */

    /* cycle check: can't move a directory into itself or its own descendant */
    if (e.type == CXFS_TYPE_DIR && cxfs_is_ancestor(id, new_parent)) return -1;

    e.parent_id = new_parent;               /* the move = one field change */
    return cxfs_write_entry(&e);
}

int cxfs_count_children(uint32_t dir_id) {
    if (!vol_select(dir_id)) return 0;
    uint32_t dir = IDX_OF(dir_id);
    int count = 0;
    for (uint32_t blk = 0; blk < vol->sb.manifest_blocks; blk++) {
        const uint8_t *buf = manifest_fetch_into(blk, dir_blk);
        if (!buf) continue;
        for (uint32_t i = 0; i < ENTRIES_PER_BLOCK; i++) {
            uint32_t cid = blk * ENTRIES_PER_BLOCK + i;
            if (cid >= vol->sb.manifest_count) return count;
            if (cid == 0) continue;
            struct cxfs_entry *e = (struct cxfs_entry *)(buf + i * sizeof(struct cxfs_entry));
            if (e->type == CXFS_TYPE_FREE) continue;
            if (e->parent_id == dir && cid != dir) count++;
        }
    }
    return count;
}

int cxfs_delete_entry(uint32_t id) {
    if (!vol_select(id)) return -1;
    if (id == TAG_ID(VOL_OF(id), vol->sb.root_id)) return -1;   /* not the root */
    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (e.type == CXFS_TYPE_FREE) return -1;

    if (e.type == CXFS_TYPE_FILE) cxfs_free_file_data(&e);

    /* Mark the manifest slot free (type 0) and write it back.
     *
     * parent_id has to carry the same volume tag as id. The memset leaves it 0,
     * which is volume 0, so on any secondary volume cxfs_write_entry's
     * cross-volume guard refused the write and the delete failed - AFTER
     * cxfs_free_file_data above had already released the blocks. The entry
     * survived still pointing at freed blocks, so the next allocation handed
     * them to another file and two entries shared data: the same capability
     * leak zero_blocks exists to prevent. (2026-10-09 review §3.) */
    memset(&e, 0, sizeof(e));
    e.id        = id;
    e.parent_id = TAG_ID(VOL_OF(id), 0);
    e.type      = CXFS_TYPE_FREE;
    return cxfs_write_entry(&e);
}

/* set an advisory lock on entry `id` for the current process. Returns 0 on
   success, -1 if it doesn't exist or is already locked by another live proc. */
int cxfs_lock(uint32_t id) {
    if (!vol_select(id)) return -1;
    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (cxfs_lock_blocks(&e)) return -1;                /* someone else holds it */
    e.lock_state     = 1;
    e.lock_owner_pid = (uint32_t)thread_current_id();
    return cxfs_write_entry(&e);
}

/* release an advisory lock. Only the lock owner or SYSTEM may unlock. */
int cxfs_unlock(uint32_t id) {
    if (!vol_select(id)) return -1;
    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (!e.lock_state) return 0;                        /* already unlocked */
    if (current_uid() != UID_SYSTEM &&
        (uint32_t)thread_current_id() != e.lock_owner_pid) return -1;
    e.lock_state = 0;
    e.lock_owner_pid = 0;
    return cxfs_write_entry(&e);
}

/* 1 if locked by another live process, else 0. */
int cxfs_is_locked(uint32_t id) {
    if (!vol_select(id)) return 0;
    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return 0;
    return cxfs_lock_blocks(&e);
}