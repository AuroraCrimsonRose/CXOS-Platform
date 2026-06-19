/* /CXLite/kernel/filesys/cxfs.c */
/* Aurora Tejeda */
/* CXFS v1 - format and mount. */

#include "cxfs.h"
#include "disk.h"
#include "string.h"

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

int cxfs_format(void) {
    if (!disk_present()) return -1;

    /* --- figure out the layout --- */
    /* the filesystem disk size: we know our image is 16 MB = 32768 blocks,
       but query conservatively by using a fixed total for v1. */
    uint32_t total_blocks = (16u * 1024 * 1024) / CXFS_BLOCK_SIZE;  /* 32768 */

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
    fs_base_lba          = 0;

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
    uint8_t block[CXFS_BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    for (uint32_t i = 0; i < bitmap_blocks; i++)
        if (write_block(bitmap_start + i, block) != 0) return -1;

    /* mark blocks 0 .. data_start-1 as used (superblock+bitmap+manifest).
       we set bits in the first bitmap block(s). */
    uint32_t used_through = data_start;    /* all metadata blocks */
    memset(block, 0, sizeof(block));
    for (uint32_t b = 0; b < used_through; b++) {
        uint32_t byte = b / 8;
        uint32_t bit  = b % 8;
        /* this simple loop assumes used_through fits in the first bitmap block
           (data_start is small: 1 + bitmap_blocks + manifest_blocks). */
        block[byte] |= (1u << bit);
    }
    if (write_block(bitmap_start, block) != 0) return -1;

    /* --- zero the manifest table --- */
    memset(block, 0, sizeof(block));
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
    memset(block, 0, sizeof(block));
    memcpy(block, &root, sizeof(root));
    if (write_block(manifest_start, block) != 0) return -1;

    mounted = 1;
    bitmap_load();
    return 0;
}

int cxfs_mount(void) {
    if (!disk_present()) return -1;

    /* The superblock is at the volume's block 0. Until we've read it we don't
       know the block geometry, but block 0 lives at base_lba sector 0; for a
       whole-disk volume that's sector 0. Read the first sector directly to get
       the superblock header, then adopt its geometry. (Partition mounts will
       pass base_lba in; for now whole-disk = 0.) */
    uint8_t first[512];
    if (disk_read(cxfs_id, fs_base_lba, 1, first) != 0) return -1;
    struct cxfs_superblock *probe = (struct cxfs_superblock *)first;
    if (probe->magic != CXFS_MAGIC)   { mounted = 0; return -1; }
    if (probe->version != CXFS_VERSION){ mounted = 0; return -1; }  /* v2 only */

    /* adopt this volume's geometry, then read the full superblock as a block. */
    fs_sectors_per_block = probe->block_size / 512;
    fs_base_lba          = probe->base_lba;
    if (read_block(0, &sb) != 0) { mounted = 0; return -1; }
    if (sb.magic != CXFS_MAGIC)  { mounted = 0; return -1; }

    mounted = 1;
    bitmap_load();
    return 0;
}

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
        uint8_t buf[CXFS_BLOCK_SIZE];
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
    uint8_t buf[CXFS_BLOCK_SIZE];
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

    uint8_t buf[CXFS_BLOCK_SIZE];
    if (read_block(blk, buf) != 0) return -1;
    memcpy(out, buf + idx * sizeof(struct cxfs_entry), sizeof(struct cxfs_entry));
    return 0;
}

int cxfs_write_entry(const struct cxfs_entry *entry) {
    if (!mounted || entry->id >= sb.manifest_count) return -1;
    uint32_t blk = sb.manifest_start + (entry->id / ENTRIES_PER_BLOCK);
    uint32_t idx = entry->id % ENTRIES_PER_BLOCK;

    uint8_t buf[CXFS_BLOCK_SIZE];
    if (read_block(blk, buf) != 0) return -1;     /* read-modify-write the block */
    memcpy(buf + idx * sizeof(struct cxfs_entry), entry, sizeof(struct cxfs_entry));
    return write_block(blk, buf);
}

int cxfs_alloc_entry(void) {
    if (!mounted) return -1;
    /* scan the manifest a whole block (4 entries) at a time to cut disk reads */
    uint8_t buf[CXFS_BLOCK_SIZE];
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
    uint8_t buf[CXFS_BLOCK_SIZE];
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
    e.size      = 0;
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
    uint8_t buf[CXFS_BLOCK_SIZE];
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

/* free every data block referenced by an entry's extents, clear them. */
void cxfs_free_file_data(struct cxfs_entry *e) {
    for (int i = 0; i < CXFS_MAX_EXTENTS; i++) {
        for (uint32_t b = 0; b < e->extent_len[i]; b++)
            cxfs_free_block(e->extent_start[i] + b);
        e->extent_start[i] = 0;
        e->extent_len[i]   = 0;
    }
    e->size = 0;
}

int cxfs_write_file(uint32_t id, const void *data, uint32_t len) {
    if (!mounted) return -1;

    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (e.type != CXFS_TYPE_FILE) return -1;

    /* release any previous content first */
    cxfs_free_file_data(&e);

    uint32_t blocks_needed = (len + CXFS_BLOCK_SIZE - 1) / CXFS_BLOCK_SIZE;
    if (len == 0) {                 /* empty file: no blocks, size 0 */
        e.size = 0;
        return cxfs_write_entry(&e);
    }

    /* allocate blocks and record them as extents. We coalesce consecutive
       block numbers into a single extent (start,len) to use few extents. */
    const uint8_t *src = (const uint8_t *)data;
    uint32_t written = 0;
    int ext = -1;                   /* current extent index */
    uint32_t prev_block = 0;

    for (uint32_t i = 0; i < blocks_needed; i++) {
        uint32_t blk = cxfs_alloc_block();
        if (blk == 0) {             /* out of space: roll back */
            cxfs_free_file_data(&e);
            return -1;
        }

        /* extend the current extent if contiguous, else open a new one */
        if (ext >= 0 && blk == prev_block + 1) {
            e.extent_len[ext]++;
        } else {
            ext++;
            if (ext >= CXFS_MAX_EXTENTS) {  /* too fragmented / too big for v1 */
                cxfs_free_block(blk);
                cxfs_free_file_data(&e);
                return -1;
            }
            e.extent_start[ext] = blk;
            e.extent_len[ext]   = 1;
        }
        prev_block = blk;

        /* write this block (zero-pad the last partial block) */
        uint8_t buf[CXFS_BLOCK_SIZE];
        uint32_t chunk = len - written;
        if (chunk > CXFS_BLOCK_SIZE) chunk = CXFS_BLOCK_SIZE;
        for (uint32_t j = 0; j < CXFS_BLOCK_SIZE; j++)
            buf[j] = (j < chunk) ? src[written + j] : 0;
        if (write_block(blk, buf) != 0) {
            cxfs_free_file_data(&e);
            return -1;
        }
        written += chunk;
    }

    e.size = len;
    return cxfs_write_entry(&e);
}

int cxfs_read_file(uint32_t id, void *buf, uint32_t cap) {
    if (!mounted) return -1;

    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return -1;
    if (e.type != CXFS_TYPE_FILE) return -1;

    uint32_t want = e.size;
    if (want > cap) want = cap;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t got = 0;

    for (int i = 0; i < CXFS_MAX_EXTENTS && got < want; i++) {
        for (uint32_t b = 0; b < e.extent_len[i] && got < want; b++) {
            uint8_t block[CXFS_BLOCK_SIZE];
            if (read_block(e.extent_start[i] + b, block) != 0) return -1;
            uint32_t chunk = want - got;
            if (chunk > CXFS_BLOCK_SIZE) chunk = CXFS_BLOCK_SIZE;
            for (uint32_t j = 0; j < chunk; j++) dst[got + j] = block[j];
            got += chunk;
        }
    }
    return (int)got;
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
    uint8_t buf[CXFS_BLOCK_SIZE];
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