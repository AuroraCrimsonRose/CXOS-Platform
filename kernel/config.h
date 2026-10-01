/* /kernel/config.h */
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

/*
 * CXK_ENABLE_FB controls the VBE linear-framebuffer path. When 1, the
 * bootloader sets a graphics mode and the kernel runs its console on the
 * framebuffer; when 0, the bootloader never touches the video hardware and the
 * kernel stays in VGA text mode end-to-end (use 0 to keep early-boot output
 * visible when debugging a fault, since text output is invisible once a
 * graphics mode is live).
 *
 * NOTE: this flag must agree with the bootloader. The real source of truth for
 * a CMake build is cmake/cxk_flags.cmake, which passes -D to BOTH gcc and nasm;
 * the #define here is only the fallback when no -D is supplied.
 */
#ifndef CXK_ENABLE_FB
#define CXK_ENABLE_FB 1             /* 1 = framebuffer console (default), 0 = force text mode */
#endif

/*
 * CXK_KTEST_STACK_OVERFLOW, when 1, ends the self-tests by overflowing a kernel
 * stack on purpose (2: thread 0's own). The boot must then stop at a red panic
 * naming the thread: proof that the guard page faulted and the double-fault
 * task caught it. See tools/cmake/cxk_flags.cmake. Never on in a build anyone
 * boots for real.
 */
#ifndef CXK_KTEST_STACK_OVERFLOW
#define CXK_KTEST_STACK_OVERFLOW 0
#endif

#endif