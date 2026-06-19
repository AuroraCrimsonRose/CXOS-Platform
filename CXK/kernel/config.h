/* /CXK/kernel/config.h */
/* Aurora Tejeda / CATX Systems LLC */
/*
 * Kernel build-time configuration / safety switches.
 *
 * CXK_ALLOW_DISK_WRITE gates EVERY code path that writes to a disk (CXFS
 * format, file writes, the filesystem self-test). It defaults to 0 (OFF) so a
 * freshly-built kernel - including any copy handed to someone else to try on
 * real hardware - is READ-ONLY to all disks and can NEVER format, overwrite, or
 * otherwise damage data on a machine it's run on.
 *
 * Auto-formatting in particular is dangerous on bare metal: a real second disk
 * (data drive, USB stick, etc.) that lacks a CXFS signature looks "unformatted"
 * and would be wiped. With this flag at 0, the kernel only MOUNTS a pre-existing
 * CXFS filesystem if it finds one, and otherwise leaves every disk untouched.
 *
 * Turn this on (set to 1) ONLY in a development build running against a
 * dedicated SCRATCH disk you are willing to erase (e.g. the QEMU/Bochs FS
 * image). Never ship a build with this enabled.
 */

#ifndef CXK_CONFIG_H
#define CXK_CONFIG_H

#ifndef CXK_ALLOW_DISK_WRITE
#define CXK_ALLOW_DISK_WRITE 1      /* 0 = read-only/safe (default), 1 = dev only */
#endif

#endif