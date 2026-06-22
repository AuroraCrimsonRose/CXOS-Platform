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

/* Copy the staged files from the STAGE partition into /System. Returns 0 on
   success (or if there is simply nothing to stage), negative on a real error. */
static int populate_from_stage(uint8_t disk_id, uint32_t system_dir) {
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

        /* create the file under /System and write the blob */
        int fid = cxfs_create_entry(system_dir, name, CXFS_TYPE_FILE);
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

    int sysdir = cxfs_create_entry(0 /* root */, "System", CXFS_TYPE_DIR);
    if (sysdir < 0) return CXK_INSTALL_FORMAT_ERR;

    if (populate_from_stage(disk_id, (uint32_t)sysdir) != 0)
        return CXK_INSTALL_STAGE_ERR;   /* formatted, but payload was missing/bad */

    return CXK_INSTALL_DONE;
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