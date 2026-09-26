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

/* CXFS operates on one registered disk, selected by its registry ID.
   default = id 0 (first registered disk). dskset changes it. */
static uint8_t cxfs_id = 0;

void    cxfs_set_id(uint8_t id) { cxfs_id = id; }
uint8_t cxfs_get_id(void)       { return cxfs_id; }

/* backwards-compatible shims (some callers still use these names) */
void cxfs_set_disk(uint8_t drive) { cxfs_id = drive; }
uint8_t cxfs_get_disk(void) { return cxfs_id; }

static int disk_present(void) {
    return disk_find_by_id(cxfs_id) != 0;
}

/* layout planning constants for a format */
#define CXFS_MAX_ENTRIES   1024            /* manifest capacity */
#define CXFS_RESERVED_KB   256             /* system-reserved data region */

static struct cxfs_superblock sb;          /* the mounted superblock */
static int mounted = 0;

static void bitmap_load(void);             /* fwd decl (defined in allocator section) */

/* ---- block staging buffers ------------------------------------------------
 * Every CXFS block operation needs a CXFS_BLOCK_SIZE (4KB) staging buffer.
 * These used to be locals, which made the frames enormous: cxfs_write_file's
 * frame measured 4432 bytes and it calls cxfs_write_entry (4160) - 8592 bytes
 * of stack against the 8192-byte THREAD_STACK that thread_alloc_kstack hands a
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
static uint32_t fs_sectors_per_block = 1;   /* CXFS_BLOCK_SIZE / 512 */
static uint64_t fs_base_lba          = 0;   /* partition offset, sectors */

static int read_block(uint32_t block, void *buf) {
    uint64_t lba = fs_base_lba + (uint64_t)block * fs_sectors_per_block;
    return disk_read(cxfs_id, lba, (uint8_t)fs_sectors_per_block, buf);
}
static int write_block(uint32_t block, const void *buf) {
    uint64_t lba = fs_base_lba + (uint64_t)block * fs_sectors_per_block;
    return disk_write(cxfs_id, lba, (uint8_t)fs_sectors_per_block, buf);
}

int cxfs_format_at(uint64_t base_lba, uint32_t total_blocks) {
    if (!disk_present()) return -1;

    /* --- figure out the layout (total_blocks = this volume's size in blocks) --- */

    uint32_t manifest_entries = CXFS_MAX_ENTRIES;
    uint32_t entries_per_block = CXFS_BLOCK_SIZE / sizeof(struct cxfs_entry); /* 4 */
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
    fs_sectors_per_block = CXFS_BLOCK_SIZE / 512;   /* 8 */
    fs_base_lba          = base_lba;

    /* --- build and write the superblock --- */
    memset(&sb, 0, sizeof(sb));
    sb.magic           = CXFS_MAGIC;
    sb.version         = CXFS_VERSION;
    sb.block_size      = CXFS_BLOCK_SIZE;
    sb.base_lba        = fs_base_lba;
    sb.total_blocks    = total_blocks;
    sb.bitmap_start    = bitmap_start;
    sb.bitmap_blocks   = bitmap_blocks;
    sb.manifest_start  = manifest_start;
    sb.manifest_blocks = manifest_blocks;
    sb.manifest_count  = manifest_entries;
    sb.data_start      = data_start;
    sb.reserved_blocks = reserved_blocks;
    sb.root_id         = 0;                /* entry 0 = root directory */
    sb.feature_flags   = CXFS_FEAT_TIMESTAMPS | CXFS_FEAT_PERMS | CXFS_FEAT_LOCKING;
    sb.entry_size      = CXFS_ENTRY_SIZE;
    sb.created         = 0;                /* timestamp wired in a later step */
    sb.modified        = 0;

    if (write_block(0, &sb) != 0) return -1;

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

    mounted = 1;
    bitmap_load();
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
       authoritative; sb.base_lba is advisory. */
    fs_base_lba = base_lba;
    uint8_t first[512];
    if (disk_read(cxfs_id, fs_base_lba, 1, first) != 0) return -1;

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
    if (probe_magic   != CXFS_MAGIC)   { mounted = 0; return -1; }
    if (probe_version != CXFS_VERSION) { mounted = 0; return -1; }  /* v2 only */
    if (probe_block_size < 512)        { mounted = 0; return -1; }

    /* adopt this volume's geometry, then read the full superblock as a block. */
    fs_sectors_per_block = probe_block_size / 512;
    /* keep fs_base_lba = base_lba (where we actually found the volume) */
    if (read_block(0, &sb) != 0) { mounted = 0; return -1; }
    if (sb.magic != CXFS_MAGIC)  { mounted = 0; return -1; }

    mounted = 1;
    bitmap_load();
    return 0;
}

int cxfs_mount(void) { return cxfs_mount_at(0); }

int cxfs_is_mounted(void) { return mounted; }

const struct cxfs_superblock *cxfs_get_superblock(void) {
    return mounted ? &sb : 0;
}

/* ====================================================================
 * Block allocator (data blocks) - bitmap cached in RAM
 *
 * The on-disk bitmap is small (a few KB). We load it into a RAM buffer at
 * mount and do all test/set against memory (instant), writing only changed
 * bitmap blocks back to disk. This avoids a disk read per bit-check, which
 * was making allocation and free-counting take seconds.
 * ==================================================================== */

#define CXFS_MAX_BITMAP_BYTES 8192   /* 32768 blocks / 8 = 4096; headroom */

static uint8_t bitmap_cache[CXFS_MAX_BITMAP_BYTES];
static uint32_t bitmap_bytes_used = 0;
static int      bitmap_loaded = 0;

/* load the whole bitmap from disk into RAM */
static void bitmap_load(void) {
    bitmap_bytes_used = (sb.total_blocks + 7) / 8;
    if (bitmap_bytes_used > CXFS_MAX_BITMAP_BYTES)
        bitmap_bytes_used = CXFS_MAX_BITMAP_BYTES;
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++) {
        uint8_t *buf = bmp_blk;
        if (read_block(sb.bitmap_start + i, buf) != 0) break;
        uint32_t off = i * CXFS_BLOCK_SIZE;
        for (uint32_t j = 0; j < CXFS_BLOCK_SIZE && off + j < bitmap_bytes_used; j++)
            bitmap_cache[off + j] = buf[j];
    }
    bitmap_loaded = 1;
}

/* write the bitmap block containing `block`'s bit back to disk */
static void bitmap_flush_for(uint32_t block) {
    uint32_t byte = block / 8;
    uint32_t which = byte / CXFS_BLOCK_SIZE;   /* which bitmap block */
    uint8_t *buf = bmp_blk;
    uint32_t off = which * CXFS_BLOCK_SIZE;
    for (uint32_t j = 0; j < CXFS_BLOCK_SIZE; j++)
        buf[j] = (off + j < bitmap_bytes_used) ? bitmap_cache[off + j] : 0;
    write_block(sb.bitmap_start + which, buf);
}

static int bitmap_test(uint32_t block) {
    uint32_t byte = block / 8, bit = block % 8;
    if (byte >= bitmap_bytes_used) return 1;
    return (bitmap_cache[byte] >> bit) & 1;
}

static void bitmap_set(uint32_t block, int used) {
    uint32_t byte = block / 8, bit = block % 8;
    if (byte >= bitmap_bytes_used) return;
    if (used) bitmap_cache[byte] |=  (1u << bit);
    else      bitmap_cache[byte] &= ~(1u << bit);
    bitmap_flush_for(block);   /* persist just the changed block */
}

uint32_t cxfs_alloc_block(void) {
    if (!mounted) return 0;
    uint32_t first = sb.data_start + sb.reserved_blocks;
    for (uint32_t b = first; b < sb.total_blocks; b++) {
        if (!bitmap_test(b)) {       /* RAM check - instant */
            bitmap_set(b, 1);        /* one disk write */
            return b;
        }
    }
    return 0;
}

void cxfs_free_block(uint32_t block) {
    if (!mounted) return;
    if (block < sb.data_start) return;
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
    if (byte >= bitmap_bytes_used) return;
    if (used) bitmap_cache[byte] |=  (1u << bit);
    else      bitmap_cache[byte] &= ~(1u << bit);
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
    if (!mounted || n == 0) return 0;
    uint32_t first = sb.data_start + sb.reserved_blocks;

    /* first-fit over the RAM bitmap: walk looking for n free in a row. On a
       used block, restart the candidate run after it. */
    uint32_t start = first, have = 0;
    for (uint32_t b = first; b < sb.total_blocks; b++) {
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

/* release `n` consecutive blocks starting at `first`, one flush per bitmap
   block rather than per released block. */
static void cxfs_free_run(uint32_t first, uint32_t n) {
    if (!mounted || n == 0) return;
    if (first < sb.data_start) return;
    for (uint32_t i = 0; i < n; i++) bitmap_mark(first + i, 0);
    bitmap_flush_span(first, first + n - 1);
}

uint32_t cxfs_free_blocks(void) {
    if (!mounted) return 0;
    uint32_t count = 0;
    for (uint32_t b = sb.data_start; b < sb.total_blocks; b++)
        if (!bitmap_test(b)) count++;   /* RAM scan - fast */
    return count;
}

/* ====================================================================
 * Manifest / entry operations
 * ==================================================================== */

#define ENTRIES_PER_BLOCK (CXFS_BLOCK_SIZE / sizeof(struct cxfs_entry))  /* 4 */

int cxfs_read_entry(uint32_t id, struct cxfs_entry *out) {
    if (!mounted || id >= sb.manifest_count) return -1;
    uint32_t blk = sb.manifest_start + (id / ENTRIES_PER_BLOCK);
    uint32_t idx = id % ENTRIES_PER_BLOCK;

    uint8_t *buf = ent_blk;
    if (read_block(blk, buf) != 0) return -1;
    memcpy(out, buf + idx * sizeof(struct cxfs_entry), sizeof(struct cxfs_entry));
    return 0;
}

int cxfs_write_entry(const struct cxfs_entry *entry) {
    if (!mounted || entry->id >= sb.manifest_count) return -1;
    uint32_t blk = sb.manifest_start + (entry->id / ENTRIES_PER_BLOCK);
    uint32_t idx = entry->id % ENTRIES_PER_BLOCK;

    uint8_t *buf = ent_blk;
    if (read_block(blk, buf) != 0) return -1;     /* read-modify-write the block */
    memcpy(buf + idx * sizeof(struct cxfs_entry), entry, sizeof(struct cxfs_entry));
    return write_block(blk, buf);
}

int cxfs_alloc_entry(void) {
    if (!mounted) return -1;
    /* scan the manifest a whole block (4 entries) at a time to cut disk reads */
    uint8_t *buf = ent_blk;
    for (uint32_t blk = 0; blk < sb.manifest_blocks; blk++) {
        if (read_block(sb.manifest_start + blk, buf) != 0) return -1;
        for (uint32_t i = 0; i < ENTRIES_PER_BLOCK; i++) {
            uint32_t id = blk * ENTRIES_PER_BLOCK + i;
            if (id == 0) continue;                 /* root */
            if (id >= sb.manifest_count) return -1;
            struct cxfs_entry *e = (struct cxfs_entry *)(buf + i * sizeof(struct cxfs_entry));
            if (e->type == CXFS_TYPE_FREE) return (int)id;
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
    if (!mounted) return -1;
    uint8_t *buf = ent_blk;
    for (uint32_t blk = 0; blk < sb.manifest_blocks; blk++) {
        if (read_block(sb.manifest_start + blk, buf) != 0) continue;
        for (uint32_t i = 0; i < ENTRIES_PER_BLOCK; i++) {
            uint32_t id = blk * ENTRIES_PER_BLOCK + i;
            if (id >= sb.manifest_count) return -1;
            struct cxfs_entry *e = (struct cxfs_entry *)(buf + i * sizeof(struct cxfs_entry));
            if (e->type == CXFS_TYPE_FREE) continue;
            if (e->parent_id == parent_id && id != parent_id &&
                name_equals(e->name, name))
                return (int)id;
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
    if (!mounted) return -1;

    /* copy + normalize the name */
    char nm[CXFS_NAME_LEN];
    int i = 0;
    for (; name[i] && i < CXFS_NAME_LEN - 1; i++) nm[i] = name[i];
    nm[i] = '\0';
    if (cxfs_normalize_name(nm) != 0) return -1;     /* bad name */

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
    if (!mounted || !path) return -1;

    uint32_t current = (path[0] == '/') ? sb.root_id : cwd;

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
            struct cxfs_entry e;
            if (cxfs_read_entry(current, &e) != 0) return -1;
            current = e.parent_id;        /* root's parent is itself */
            continue;
        }

        int found = cxfs_find_in_dir(current, comp);
        if (found < 0) return -1;         /* component doesn't exist */
        current = (uint32_t)found;
    }
    return (int)current;
}

void cxfs_list_dir(uint32_t parent_id, void (*cb)(const struct cxfs_entry *)) {
    if (!mounted || !cb) return;
    uint8_t *buf = dir_blk;   /* not ent_blk: cb() may read entries of its own */
    for (uint32_t blk = 0; blk < sb.manifest_blocks; blk++) {
        if (read_block(sb.manifest_start + blk, buf) != 0) continue;
        for (uint32_t i = 0; i < ENTRIES_PER_BLOCK; i++) {
            uint32_t id = blk * ENTRIES_PER_BLOCK + i;
            if (id >= sb.manifest_count) return;
            if (id == 0) continue;        /* skip root itself */
            struct cxfs_entry *e = (struct cxfs_entry *)(buf + i * sizeof(struct cxfs_entry));
            if (e->type == CXFS_TYPE_FREE) continue;
            if (e->parent_id == parent_id) cb(e);
        }
    }
}

void cxfs_path_of(uint32_t id, char *out, int cap) {
    if (cap <= 0) return;

    /* root is just "/" */
    if (id == sb.root_id) { if (cap > 1) { out[0] = '/'; out[1] = '\0'; } else out[0] = '\0'; return; }

    /* collect names from `id` up to root, then emit reversed */
    char names[16][CXFS_NAME_LEN];   /* up to 16 levels deep */
    int depth = 0;
    uint32_t cur = id;
    while (cur != sb.root_id && depth < 16) {
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
    if (!mounted) return -1;

    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (e.type != CXFS_TYPE_FILE) return -1;

    if (!cxfs_check_perm(&e, CXFS_ACC_READ)) return -1;    /* v2: permission */

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
    if (!mounted) return -1;
    int id = cxfs_resolve(path, 0);          /* 0 = root; absolute paths */
    if (id < 0) return -1;
    return cxfs_read_entry((uint32_t)id, out);
}

int cxfs_read_path(const char *path, void *buf, uint32_t cap) {
    if (!mounted) return -1;
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
        if (bi < seen + e->extent_len[i]) return e->extent_start[i] + (bi - seen);
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
    if (!mounted) return CXFS_E_FAIL;
    if (cxfs_read_entry(id, e) != 0)        return CXFS_E_NOTFOUND;
    if (e->type == CXFS_TYPE_DIR)           return CXFS_E_ISDIR;
    if (e->type != CXFS_TYPE_FILE)          return CXFS_E_INVAL;
    if (!cxfs_check_perm(e, CXFS_ACC_WRITE)) return CXFS_E_PERM;
    if (cxfs_lock_blocks(e))                return CXFS_E_LOCKED;
    return CXFS_E_OK;
}

int cxfs_read_at(uint32_t id, uint64_t off, void *buf, uint32_t len) {
    if (!mounted) return CXFS_E_FAIL;
    if (!buf)     return CXFS_E_INVAL;

    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0)      return CXFS_E_NOTFOUND;
    if (e.type == CXFS_TYPE_DIR)           return CXFS_E_ISDIR;
    if (e.type != CXFS_TYPE_FILE)          return CXFS_E_INVAL;
    if (!cxfs_check_perm(&e, CXFS_ACC_READ)) return CXFS_E_PERM;

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
    if (!mounted) return CXFS_E_FAIL;
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
    if (!mounted) return -1;
    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (id == sb.root_id) return -1;        /* don't rename root */

    char nm[CXFS_NAME_LEN];
    int i = 0;
    for (; newname[i] && i < CXFS_NAME_LEN - 1; i++) nm[i] = newname[i];
    nm[i] = '\0';
    if (cxfs_normalize_name(nm) != 0) return -1;

    strlcpy(e.name, nm, CXFS_NAME_LEN);
    return cxfs_write_entry(&e);
}

int cxfs_is_ancestor(uint32_t ancestor, uint32_t id) {
    /* walk up from id to root; if we meet `ancestor`, it's an ancestor. */
    uint32_t cur = id;
    for (int guard = 0; guard < 1024; guard++) {
        if (cur == ancestor) return 1;
        if (cur == sb.root_id) return 0;
        struct cxfs_entry e;
        if (cxfs_read_entry(cur, &e) != 0) return 0;
        if (e.parent_id == cur) return 0;   /* root safety */
        cur = e.parent_id;
    }
    return 0;
}

int cxfs_move(uint32_t id, uint32_t new_parent) {
    if (!mounted) return -1;
    if (id == sb.root_id) return -1;        /* can't move root */

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
    if (!mounted) return 0;
    int count = 0;
    uint8_t *buf = dir_blk;
    for (uint32_t blk = 0; blk < sb.manifest_blocks; blk++) {
        if (read_block(sb.manifest_start + blk, buf) != 0) continue;
        for (uint32_t i = 0; i < ENTRIES_PER_BLOCK; i++) {
            uint32_t cid = blk * ENTRIES_PER_BLOCK + i;
            if (cid >= sb.manifest_count) return count;
            if (cid == 0) continue;
            struct cxfs_entry *e = (struct cxfs_entry *)(buf + i * sizeof(struct cxfs_entry));
            if (e->type == CXFS_TYPE_FREE) continue;
            if (e->parent_id == dir_id && cid != dir_id) count++;
        }
    }
    return count;
}

int cxfs_delete_entry(uint32_t id) {
    if (!mounted || id == sb.root_id) return -1;
    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (e.type == CXFS_TYPE_FREE) return -1;

    if (e.type == CXFS_TYPE_FILE) cxfs_free_file_data(&e);

    /* mark the manifest slot free (type 0) and write it back */
    memset(&e, 0, sizeof(e));
    e.id   = id;
    e.type = CXFS_TYPE_FREE;
    return cxfs_write_entry(&e);
}

/* set an advisory lock on entry `id` for the current process. Returns 0 on
   success, -1 if it doesn't exist or is already locked by another live proc. */
int cxfs_lock(uint32_t id) {
    if (!mounted) return -1;
    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (cxfs_lock_blocks(&e)) return -1;                /* someone else holds it */
    e.lock_state     = 1;
    e.lock_owner_pid = (uint32_t)thread_current_id();
    return cxfs_write_entry(&e);
}

/* release an advisory lock. Only the lock owner or SYSTEM may unlock. */
int cxfs_unlock(uint32_t id) {
    if (!mounted) return -1;
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
    if (!mounted) return 0;
    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return 0;
    return cxfs_lock_blocks(&e);
}