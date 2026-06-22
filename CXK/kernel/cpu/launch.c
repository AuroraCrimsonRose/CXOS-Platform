/* /CXK/kernel/cpu/launch.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* Read a CXEX image from CXFS and run it via cxex_exec. See launch.h. */

#include "launch.h"
#include "exec.h"
#include "cxfs.h"
#include "heap.h"
#include <stdint.h>
#include <stddef.h>

int cxk_launch_executive(const char *path) {
    struct cxfs_entry e;
    if (cxfs_stat_path(path, &e) != 0)             return CXK_LAUNCH_NOT_FOUND;
    if (e.type != CXFS_TYPE_FILE || e.size == 0)   return CXK_LAUNCH_NOT_FOUND;

    uint8_t *buf = (uint8_t *)kmalloc((size_t)e.size);
    if (!buf)                                      return CXK_LAUNCH_NOMEM;

    int rd = cxfs_read_path(path, buf, (uint32_t)e.size);
    if (rd != (int)e.size) { kfree(buf);           return CXK_LAUNCH_READ_ERR; }

    int rc = cxex_exec(buf, (size_t)e.size);       /* verify -> space -> load -> ring 3 */
    kfree(buf);
    return rc;
}