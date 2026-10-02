/* /kernel/cpu/launch.h */
/* Aurora Tejeda / CATX Systems */
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

/* Human-readable reason for a cxk_launch_executive() failure. Spans BOTH error
   spaces - launch.h's own codes and the cxex_exec codes in exec.h - because
   cxk_launch_executive returns either and the caller cannot tell them apart
   from the number alone. */
const char *cxk_launch_strerror(int rc);

#endif