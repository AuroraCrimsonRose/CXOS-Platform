/* /CXK/kernel/drivers/storage/install.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* First-boot install: format SYSTEM + populate /System from STAGE. See install.h. */

#include "install.h"
#include "partition.h"
#include "disk.h"
#include "cxfs.h"
#include "heap.h"

#define SECTOR 512u
#define CXFS_BLOCK 4096u

static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p){ return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24)); }

/* ---- the system tree -------------------------------------------------------
 * The directory layout a fresh volume gets, per docs/CX_FILESYSTEM_LAYOUT.md.
 *
 * /System is the OS and only SYSTEM writes it. /Shared is group-writable, so a
 * user can put something there for other users without being SYSTEM - that is
 * a split by PROTECTION, not by trust: signing governs what may run, these
 * bits govern what may be placed. /Temp is world-writable scratch, and
 * /System/Temp is separate from it so the OS never contends with a user
 * program for space or for names.
 *
 * /System/Drivers is NOT here. A directory costs a manifest entry permanently
 * and tells a reader something exists; it gets created when there is a driver
 * to put in it. Same reasoning for the per-user directories under /User, which
 * belong to the account layer that does not exist yet.
 */
struct tree_dir {
    const char *path;
    uint16_t    mode;
};

static const struct tree_dir system_tree[] = {
    { "/System",           CXFS_PERM_DIR_DEFAULT },              /* 0755 */
    { "/System/Programs",  CXFS_PERM_DIR_DEFAULT },
    { "/System/Kernel",    CXFS_PERM_DIR_DEFAULT },              /* update staging */
    { "/System/Boot",      CXFS_PERM_DIR_DEFAULT },              /* update staging */
    { "/System/Services",  CXFS_PERM_DIR_DEFAULT },              /* .xosv descriptors */
    { "/System/Temp",      CXFS_PERM_DIR_DEFAULT },
    { "/Shared",           CXFS_PERM_DIR_DEFAULT | CXFS_PERM_GW },  /* 0775 */
    { "/Shared/Programs",  CXFS_PERM_DIR_DEFAULT | CXFS_PERM_GW },
    { "/Shared/Documents", CXFS_PERM_DIR_DEFAULT | CXFS_PERM_GW },
    { "/Shared/Pictures",  CXFS_PERM_DIR_DEFAULT | CXFS_PERM_GW },
    { "/Shared/Audio",     CXFS_PERM_DIR_DEFAULT | CXFS_PERM_GW },
    { "/Shared/Videos",    CXFS_PERM_DIR_DEFAULT | CXFS_PERM_GW },
    { "/User",             CXFS_PERM_DIR_DEFAULT },
    { "/Temp",             CXFS_PERM_DIR_DEFAULT | CXFS_PERM_GW | CXFS_PERM_TW }, /* 0777 */
    { "/Drives",           CXFS_PERM_DIR_DEFAULT },
};

/* Find or create one directory named `name` inside `parent`. The single-
   component sibling of ensure_dir, for a name that comes from data rather
   than from a path literal - a drive's registry name, say. Returns the entry
   id, or negative if it could not be made or something that is not a
   directory is already sitting there. */
static int ensure_dir_in(uint32_t parent, const char *name) {
    int found = cxfs_find_in_dir(parent, name);
    if (found < 0) return cxfs_create_entry(parent, name, CXFS_TYPE_DIR);

    struct cxfs_entry e;
    if (cxfs_read_entry((uint32_t)found, &e) != 0) return -1;
    if (e.type != CXFS_TYPE_DIR) return -1;   /* a file is in the way */
    return found;
}

/* Resolve `path` as a directory, creating any component that is missing.
   Idempotent - an existing directory is returned, not recreated. Returns the
   entry id, or negative. */
static int ensure_dir(const char *path) {
    uint32_t cur = 0;                      /* root */
    const char *p = path;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char comp[CXFS_NAME_LEN];
        uint32_t n = 0;
        while (*p && *p != '/' && n < CXFS_NAME_LEN - 1) comp[n++] = *p++;
        comp[n] = '\0';
        while (*p && *p != '/') p++;       /* a name too long to hold is an error */
        if (*p == '/') p++;

        int found = cxfs_find_in_dir(cur, comp);
        if (found < 0) {
            found = cxfs_create_entry(cur, comp, CXFS_TYPE_DIR);
            if (found < 0) return -1;
        } else {
            struct cxfs_entry e;
            if (cxfs_read_entry((uint32_t)found, &e) != 0) return -1;
            if (e.type != CXFS_TYPE_DIR) return -1;   /* a file is in the way */
        }
        cur = (uint32_t)found;
    }
    return (int)cur;
}

/* Build the directory tree on a freshly formatted volume. */
static int create_system_tree(void) {
    for (unsigned i = 0; i < sizeof system_tree / sizeof system_tree[0]; i++) {
        int id = ensure_dir(system_tree[i].path);
        if (id < 0) return -1;

        struct cxfs_entry e;
        if (cxfs_read_entry((uint32_t)id, &e) != 0) return -1;
        if (e.permissions != system_tree[i].mode) {
            e.permissions = system_tree[i].mode;
            if (cxfs_write_entry(&e) != 0) return -1;
        }
    }
    return 0;
}

/* Copy the staged files from the STAGE partition into the tree. A staged name
   is a PATH relative to the root ("Shared/Programs/hi.xuex"), not a bare leaf
   name, so the build says where each file goes instead of install.c guessing
   from the extension. Any missing parent is created.

   Returns 0 on success (or if there is simply nothing to stage), negative on a
   real error. */
static int populate_from_stage(uint8_t disk_id) {
    struct partition stage;
    if (part_find_type(disk_id, PART_TYPE_CXSTAGE, &stage) != 0)
        return 0;                               /* no staging area: empty /System is fine */

    uint8_t hdr[SECTOR];
    if (disk_read(disk_id, stage.start_lba, 1, hdr) != 0) return -1;
    if (hdr[0]!='X'||hdr[1]!='S'||hdr[2]!='T'||hdr[3]!='G') return -1;
    if (rd16(hdr + 4) != XSTG_VERSION) return -1;

    uint16_t count = rd16(hdr + 6);
    if (count > XSTG_MAX_FILES) count = XSTG_MAX_FILES;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *e = hdr + 16 + (uint32_t)i * XSTG_ENTRY_SIZE;
        char name[XSTG_NAME_LEN + 1];
        for (uint32_t k = 0; k < XSTG_NAME_LEN; k++) name[k] = (char)e[k];
        name[XSTG_NAME_LEN] = '\0';
        uint32_t start_sector = rd32(e + 32);
        uint32_t size         = rd32(e + 44);
        if (name[0] == '\0' || size == 0) continue;

        /* read the blob (sector-aligned) into a heap buffer */
        uint32_t sectors = (size + SECTOR - 1) / SECTOR;
        uint8_t *buf = (uint8_t *)kmalloc(sectors * SECTOR);
        if (!buf) return -1;
        if (disk_read(disk_id, stage.start_lba + start_sector, sectors, buf) != 0) {
            kfree(buf);
            return -1;
        }

        /* split "Shared/Programs/hi.xuex" into its directory and its leaf */
        int slash = -1;
        for (int k = 0; name[k]; k++) if (name[k] == '/') slash = k;

        uint32_t dir = 0;                       /* no slash: straight in the root */
        if (slash >= 0) {
            char dirpath[XSTG_NAME_LEN + 1];
            for (int k = 0; k < slash; k++) dirpath[k] = name[k];
            dirpath[slash] = '\0';
            int d = ensure_dir(dirpath);
            if (d < 0) { kfree(buf); return -1; }
            dir = (uint32_t)d;
        }
        const char *leaf = name + slash + 1;
        if (!*leaf) { kfree(buf); return -1; }

        int fid = cxfs_create_entry(dir, leaf, CXFS_TYPE_FILE);
        if (fid < 0 || cxfs_write_file((uint32_t)fid, buf, size) != 0) {
            kfree(buf);
            return -1;
        }
        kfree(buf);
    }
    return 0;
}

int cxk_install_first_boot(uint8_t disk_id) {
    struct partition sysp;
    if (part_find_type(disk_id, PART_TYPE_CXFS, &sysp) != 0)
        return CXK_INSTALL_NO_SYSTEM;

    /* already installed? then just mount and we're done. */
    if (cxfs_mount_at(sysp.start_lba) == 0)
        return CXK_INSTALL_MOUNTED;

    /* fresh disk: format the SYSTEM partition, mount it, create /System. */
    uint32_t total_blocks = (uint32_t)(sysp.sectors / (CXFS_BLOCK / SECTOR));
    if (cxfs_format_at(sysp.start_lba, total_blocks) != 0) return CXK_INSTALL_FORMAT_ERR;
    if (cxfs_mount_at(sysp.start_lba) != 0)                return CXK_INSTALL_FORMAT_ERR;

    if (create_system_tree() != 0) return CXK_INSTALL_FORMAT_ERR;

    if (populate_from_stage(disk_id) != 0)
        return CXK_INSTALL_STAGE_ERR;   /* formatted, but payload was missing/bad */

    return CXK_INSTALL_DONE;
}

/* ---- drives and their volumes ----------------------------------------------
 * A DRIVE is the hardware - a disk the machine has. A VOLUME is a filesystem
 * ON a drive, either filling it or sitting in one of its partitions. The tree
 * says so: /Drives holds one directory per drive, named as the disk registry
 * names it (HDD0, HDD1, ...), and a drive's volumes are the directories inside
 * it, named by their own labels.
 *
 * So a file reads /Drives/HDD1/Data/notes.txt - drive, then volume, then path.
 * That is one component longer than hanging every volume off a single
 * directory, and it is worth it: two drives may each hold a volume called
 * Data, and with a flat namespace the second one has nowhere to go.
 *
 * The scan is deliberately dumb. Try the whole drive as one volume first; if
 * that is not CXFS, try each partition an XBPT table declares. A drive holding
 * neither is not mounted and that is not an error - a disk with a filesystem
 * this kernel does not know is simply not ours to mount.
 *
 * Each volume's own mount point is created by cxfs_mount_volume rather than
 * here, because its name is the volume's label and that is not known until the
 * superblock has been read.
 */
int cxk_mount_extra_volumes(uint8_t boot_disk_id) {
    int mounted = 0;

    int drives_dir = cxfs_resolve("/Drives", 0);
    if (drives_dir < 0) return 0;        /* no /Drives: nothing to mount into */

    unsigned n = disk_count();
    for (unsigned i = 0; i < n; i++) {
        const struct disk *d = disk_get(i);
        if (!d || d->id == boot_disk_id) continue;

        /* The drive's own directory. Made before probing, and left behind if
           the drive turns out to hold nothing we can mount: an empty
           /Drives/HDD1 says "this drive is here and has no CXFS volume on it",
           which is more use to someone looking than no entry at all. */
        int dd = ensure_dir_in(drives_dir, d->name);
        if (dd < 0) continue;

        if (cxfs_mount_volume(d->id, 0, (uint32_t)dd, 0) >= 0) {
            mounted++;
            continue;                    /* whole-drive volume: done with it */
        }

        struct partition parts[XBPT_MAX_ENTRIES];
        int cnt = 0;
        if (part_scan(d->id, parts, XBPT_MAX_ENTRIES, &cnt) != 0) continue;
        for (int p = 0; p < cnt; p++)
            if (cxfs_mount_volume(d->id, parts[p].start_lba,
                                  (uint32_t)dd, 0) >= 0)
                mounted++;
    }
    return mounted;
}

int cxk_install_boot_disk(void) {
    /* find the disk carrying an XBPT table, point CXFS at it, and install. */
    unsigned n = disk_count();
    for (unsigned i = 0; i < n; i++) {
        const struct disk *d = disk_get(i);
        if (!d) continue;
        struct partition parts[XBPT_MAX_ENTRIES];
        int cnt = 0;
        if (part_scan(d->id, parts, XBPT_MAX_ENTRIES, &cnt) == 0 && cnt > 0) {
            cxfs_set_id(d->id);
            return cxk_install_first_boot(d->id);
        }
    }
    return CXK_INSTALL_NO_SYSTEM;   /* no XBPT disk present */
}