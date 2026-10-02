/* /kernel/cpu/launch.c */
/* Aurora Tejeda / CATX Systems */
/* Read a CXEX image from CXFS and run it via cxex_exec. See launch.h. */

#include "launch.h"
#include "exec.h"
#include "cxfs.h"
#include "heap.h"
#include <stdint.h>
#include <stddef.h>

const char *cxk_launch_strerror(int rc) {
    switch (rc) {
        case CXK_LAUNCH_NOT_FOUND:
            return "/System/Boot.xoex is missing or not a file";
        case CXK_LAUNCH_NOMEM:
            return "out of memory reading the image";
        case CXK_LAUNCH_READ_ERR:
            return "short read from CXFS";
        /* By far the most common one, and the least obvious: an UNSIGNED build
           produces an executive the kernel is right to refuse. Say so, because
           the bare code looks like corruption rather than policy working. */
        case CXEX_EXEC_VERIFY_FAILED:
            return "signature check failed - unsigned, or signed by a key this kernel "
                   "does not trust. An unsigned build is the usual cause: rebuild with "
                   "SIGN=ON";
        case CXEX_EXEC_BAD_TYPE:
            return "not an OS or USER executable";
        case CXEX_EXEC_NOSPACE:
            return "could not create an address space";
        case CXEX_EXEC_LOAD_FAILED:
            return "malformed CXEX - the image failed to load";
        case CXEX_EXEC_NOMEM:
            return "could not allocate the user stack";
        default:
            return "unrecognised error";
    }
}

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