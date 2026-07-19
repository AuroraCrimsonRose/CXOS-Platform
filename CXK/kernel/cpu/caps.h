/* /CXK/kernel/cpu/caps.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * CXK ABI v1 - capabilities + error codes (see docs/CXK_ABI_v1).
 *
 * A capability is authority to use a CLASS of kernel primitive. Caps live in
 * the kernel's per-process record (struct thread), never in user memory, so
 * ring 3 cannot forge them. The syscall dispatcher gates a privileged call with
 * a single check:  if (!(thread_current_caps() & CAP_X)) return E_PERM;
 *
 * In the brokered ("B") model only the .xoex executive holds privileged caps;
 * apps hold none and reach privilege only by IPC to their executive. Authority
 * is granted once, at the cxex_exec handoff, by caps_for() below - the signature
 * established identity; this maps identity -> capability tier.
 */

#ifndef CAPS_H
#define CAPS_H

#include <stdint.h>
#include "cxk_abi.h"   /* E_* codes + the public ABI contract */

/* capability bits (per-process bitmask) */
#define CAP_CONSOLE   0x0001u   /* console_write */
#define CAP_MEM       0x0002u   /* map / unmap / sbrk */
#define CAP_DISK      0x0004u   /* block_read / block_write */
#define CAP_NET       0x0008u   /* SYS_NET_OP: interface config, ping, raw frames */
#define CAP_SPAWN     0x0010u   /* spawn */
#define CAP_POWER     0x0020u   /* power (reboot/shutdown) */
#define CAP_ENDPOINT  0x0040u   /* ep_create (may own an IPC endpoint -> broker) */
#define CAP_IOPORT    0x0080u   /* (reserved v1) raw port I/O / driver tier */
#define CAP_FRAMEBUFFER 0x0100u /* SYS_FB_OP: draw to the framebuffer (in baseline
                                   for now; split to a display-server tier later) */

/* a broker executive's baseline authority */
/* The ABI (abi/cxk_abi.h) carries a copy of these so userspace spawners can
   request caps. The two MUST agree - guarded here so including both is
   silent, and checked below so a future edit to one can't drift. */
#ifndef CAP_OS_BASELINE
#define CAP_OS_BASELINE \
    (CAP_CONSOLE | CAP_MEM | CAP_DISK | CAP_SPAWN | CAP_POWER | CAP_ENDPOINT | CAP_FRAMEBUFFER | CAP_NET)
#endif


/* policy layer: map a verified image's identity (CXEX type code + trust) to its
   capability set. Consulted once, at the ring-3 handoff. A valid signature does
   NOT itself grant authority - it only lets us look the identity up here. */
uint32_t caps_for(uint16_t type_code, int trusted);

#endif