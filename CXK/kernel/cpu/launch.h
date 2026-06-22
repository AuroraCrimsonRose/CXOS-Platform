/* /CXK/kernel/cpu/launch.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * cxk_launch_executive: read a signed CXEX image out of the mounted CXFS and
 * hand it to cxex_exec (verify -> own address space -> load -> ring 3). This is
 * the capstone the kernel uses at boot to start the system executive from
 * /System. Returns the executive's exit code (>=0), or a negative error.
 */

#ifndef LAUNCH_H
#define LAUNCH_H

enum cxk_launch_result {
    CXK_LAUNCH_NOT_FOUND = -10,   /* path missing / not a file */
    CXK_LAUNCH_NOMEM     = -11,   /* could not allocate the read buffer */
    CXK_LAUNCH_READ_ERR  = -12    /* short read */
    /* >=0 : executive exit code; -1..-5 : cxex_exec errors (see exec.h) */
};

int cxk_launch_executive(const char *path);

#endif