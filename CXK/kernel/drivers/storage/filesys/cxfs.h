/* /CXLite/kernel/filesys/cxfs.h */
/* Aurora Tejeda */
/*
 * CXFS - the CXOS filesystem. v1.
 *
 * A manifest-based filesystem: the on-disk "manifest" (a table of entries)
 * is the source of truth; the directory tree is derived from parent_id links.
 * Moving a file changes its parent_id only - the data never moves.
 *
 * v1 implements: format, hierarchical directories, extent-based file storage,
 * case-insensitive (case-preserving) names. Fields for ownership, groups,
 * permissions, and locks are RESERVED in the entry now (zeroed) and enforced
 * later (v2) once a user/process model exists.
 *
 * Sits on top of the ATA driver (operates on the filesystem disk, drive 1).
*/
/*
 * This all took an enourmous amount of planning on my behest i took a whole
 * afternoon piecing together a plan, and structure for this so im happy to 
 * say the final result should be fairly sound
*/

#ifndef CXFS_H
#define CXFS_H

#include <stdint.h>

#define CXFS_MAGIC        0x43584653u   /* "CXFS" */
#define CXFS_VERSION      2
#define CXFS_BLOCK_SIZE   4096          /* v2: 4KB page-aligned blocks */
#define CXFS_NAME_LEN     64
#define CXFS_MAX_EXTENTS  8             /* v2: 8 extents */
#define CXFS_ENTRY_SIZE   256           /* v2: 256-byte manifest entries */
#define CXFS_DISK_DEFAULT 1            /* default ata drive (primary slave) */

/* v2 feature flags (superblock feature_flags) */
#define CXFS_FEAT_TIMESTAMPS 0x01
#define CXFS_FEAT_PERMS      0x02
#define CXFS_FEAT_LOCKING    0x04
#define CXFS_FEAT_LARGE_BLK  0x08

/* permission bits (low 9 of `permissions`): rwx for owner/group/other */
#define CXFS_PERM_OR  0x100   /* owner read  */
#define CXFS_PERM_OW  0x080   /* owner write */
#define CXFS_PERM_OX  0x040   /* owner exec  */
#define CXFS_PERM_GR  0x020
#define CXFS_PERM_GW  0x010
#define CXFS_PERM_GX  0x008
#define CXFS_PERM_TR  0x004
#define CXFS_PERM_TW  0x002
#define CXFS_PERM_TX  0x001
#define CXFS_PERM_FILE_DEFAULT 0x1A4   /* 0644 rw-r--r-- */
#define CXFS_PERM_DIR_DEFAULT  0x1ED   /* 0755 rwxr-xr-x */

/* access kinds for permission checks */
#define CXFS_ACC_READ  0
#define CXFS_ACC_WRITE 1
#define CXFS_ACC_EXEC  2
/* The active CXFS target drive is runtime-settable (so the same build works
   on emulators, where the fs disk is drive 1, and on real hardware where it
   may be on another channel/port, e.g. drive 3). */

/* entry types */
#define CXFS_TYPE_FREE    0
#define CXFS_TYPE_FILE    1
#define CXFS_TYPE_DIR     2

/* superblock - block 0 - the master manifest header */
struct cxfs_superblock {
    uint32_t magic;            /* CXFS_MAGIC */
    uint16_t version;          /* CXFS_VERSION (2) */
    uint16_t block_size;       /* 4096 (authoritative) */
    uint64_t base_lba;         /* partition offset in 512B sectors; 0 = whole disk */
    uint32_t total_blocks;     /* total blocks in the volume */
    uint32_t bitmap_start;     /* first block of the allocation bitmap */
    uint32_t bitmap_blocks;    /* blocks used by the bitmap */
    uint32_t manifest_start;   /* first block of the manifest table */
    uint32_t manifest_blocks;  /* blocks used by the manifest */
    uint32_t manifest_count;   /* max entries */
    uint32_t data_start;       /* first data block */
    uint32_t reserved_blocks;  /* system-reserved data blocks */
    uint32_t root_id;          /* manifest id of the root directory (0) */
    uint32_t feature_flags;    /* CXFS_FEAT_* active features */
    uint64_t created;          /* volume creation timestamp */
    uint64_t modified;         /* superblock last-write timestamp */
    uint32_t entry_size;       /* bytes per manifest entry (256) */
    uint8_t  pad[CXFS_BLOCK_SIZE - 76];  /* fill the 4KB block */
} __attribute__((packed));

/* one manifest entry - a file or directory. 256 bytes (v2). */
struct cxfs_entry {
    uint32_t id;                          /* this entry's id (table index) */
    uint32_t parent_id;                   /* containing dir's id */
    uint8_t  type;                        /* CXFS_TYPE_* */
    uint8_t  flags;                       /* misc flags */
    uint8_t  name_len;                    /* length of name */
    uint8_t  reserved0;                   /* align */
    char     name[CXFS_NAME_LEN];         /* 64, case-preserved */
    uint64_t size;                        /* file size in bytes (64-bit) */
    uint32_t extent_start[CXFS_MAX_EXTENTS]; /* 8 extent start blocks */
    uint32_t extent_len[CXFS_MAX_EXTENTS];   /* 8 extent lengths (blocks) */
    uint32_t owner_uid;                   /* 0 = SYSTEM */
    uint32_t group_id;                    /* 0 = system group */
    uint16_t permissions;                 /* rwx owner/group/other */
    uint8_t  lock_state;                  /* 0 unlocked, 1 advisory write-lock */
    uint8_t  lock_pad;
    uint32_t lock_owner_pid;              /* process holding the lock (0 = none) */
    uint64_t created;                     /* timestamps (epoch seconds) */
    uint64_t modified;
    uint64_t accessed;
    uint8_t  pad1[CXFS_ENTRY_SIZE - 188]; /* pad to 256 bytes */
} __attribute__((packed));

/* initialize a blank CXFS on the filesystem disk. returns 0 on success. */
int cxfs_format(void);

/* mount: read the superblock, verify magic. returns 0 on success, -1 if no
   valid CXFS found (e.g. unformatted disk). */
int cxfs_mount(void);

/* select / query which ATA drive CXFS operates on (0-3). */
void    cxfs_set_disk(uint8_t drive);
uint8_t cxfs_get_disk(void);

/* select the CXFS volume by unified disk-registry ID (see the disks command) */
void    cxfs_set_id(uint8_t id);
uint8_t cxfs_get_id(void);

/* is a valid CXFS currently mounted? */
int cxfs_is_mounted(void);

/* accessors for the mounted superblock (for stat/debug commands) */
const struct cxfs_superblock *cxfs_get_superblock(void);

/* --- block allocator (data blocks, via the bitmap) --- */
/* allocate one free data block; returns its block number, or 0 on failure
   (0 is never a valid data block since it's the superblock). */
uint32_t cxfs_alloc_block(void);
/* free a previously allocated data block. */
void     cxfs_free_block(uint32_t block);
/* count free data blocks (for stats). */
uint32_t cxfs_free_blocks(void);

/* --- manifest / entry operations --- */
/* read entry `id` from disk into `out`. returns 0 on success. */
int cxfs_read_entry(uint32_t id, struct cxfs_entry *out);
/* write `entry` back to its slot (entry->id) on disk. returns 0 on success. */
int cxfs_write_entry(const struct cxfs_entry *entry);
/* allocate a free manifest slot, returns its id, or -1 if the manifest is full. */
int cxfs_alloc_entry(void);
/* find a child named `name` (case-insensitive) inside directory `parent_id`.
   returns the child's id, or -1 if not found. */
int cxfs_find_in_dir(uint32_t parent_id, const char *name);

/* normalize a filename in place: spaces -> '_'. returns 0 if valid,
   -1 if it contains an illegal character ('/', control chars) or is empty. */
int cxfs_normalize_name(char *name);

/* --- directory operations + path resolution --- */

/* create a file or directory named `name` inside directory `parent_id`.
   returns the new entry's id, or -1 on error (exists, full, bad name). */
int cxfs_create_entry(uint32_t parent_id, const char *name, uint8_t type);

/* resolve a path to an entry id. absolute (/a/b) or relative to `cwd`.
   supports "." and "..". returns the entry id, or -1 if not found. */
int cxfs_resolve(const char *path, uint32_t cwd);

/* call `cb` once per entry whose parent is `parent_id` (for listings). */
void cxfs_list_dir(uint32_t parent_id, void (*cb)(const struct cxfs_entry *));

/* build the absolute path of entry `id` into `out` (size `cap`). */
void cxfs_path_of(uint32_t id, char *out, int cap);

/* --- file content (extent-based) --- */

/* write `len` bytes from `data` as the entire contents of file `id`,
   replacing any previous content. Allocates blocks, records extents.
   returns 0 on success, -1 on error (too big for v1's extents, etc.). */
int cxfs_write_file(uint32_t id, const void *data, uint32_t len);

/* read up to `cap` bytes of file `id` into `buf`. returns bytes read,
   or -1 on error. */
int cxfs_read_file(uint32_t id, void *buf, uint32_t cap);

/* free all data blocks of an entry and clear its extents (size -> 0). */
void cxfs_free_file_data(struct cxfs_entry *e);

/* --- rename / move / delete --- */

/* rename entry `id` to `newname` (in place; parent unchanged).
   returns 0 on success, -1 on error (bad name, etc.). does NOT check
   for collisions - the caller (shell) handles that with its flags. */
int cxfs_rename(uint32_t id, const char *newname);

/* move entry `id` into directory `new_parent`. returns 0 on success,
   -1 on error (not a dir, cycle, etc.). */
int cxfs_move(uint32_t id, uint32_t new_parent);

/* is `ancestor` an ancestor of (or equal to) `id`? used for cycle checks. */
int cxfs_is_ancestor(uint32_t ancestor, uint32_t id);

/* count children of a directory (0 = empty). */
int cxfs_count_children(uint32_t dir_id);

/* delete a single entry: free its data + mark its manifest slot free.
   does NOT recurse (caller handles recursion). returns 0 on success. */
int cxfs_delete_entry(uint32_t id);


/* v2 advisory locking */
int cxfs_lock(uint32_t id);
int cxfs_unlock(uint32_t id);
int cxfs_is_locked(uint32_t id);

#endif