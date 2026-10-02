// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/caps.h */
/* Aurora Tejeda / CATX Systems */
/*
 * CXK ABI v1 - capabilities + error codes (see docs/CXK_ABI_v1).
 *
 * A capability is authority to use a CLASS of kernel primitive: what a process
 * is ALLOWED TO DO, held as a bitmask. The bits are named GRANT_* because that
 * is what each one is - a grant, handed to a process when it was started and
 * never larger than what the thing that started it already held.
 *
 * They live in the kernel's per-process record (struct thread), never in user
 * memory, so ring 3 cannot forge them. The syscall dispatcher gates a
 * privileged call with a single check:
 *
 *     if (!(thread_current_caps() & GRANT_X)) return E_PERM;
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

/* The grants, one bit each. A process holds some subset of these; a launcher
   passes a subset of its own to a child and can never amplify (see the
   attenuation note in docs/kernel/CX_ABI.md section 5). */
#define GRANT_CONSOLE   0x0001u   /* console_write */
#define GRANT_MEM       0x0002u   /* map / unmap / sbrk */
#define GRANT_DISK      0x0004u   /* block_read / block_write */
#define GRANT_NET       0x0008u   /* SYS_NET_OP: interface config, ping, raw frames */
#define GRANT_SPAWN     0x0010u   /* spawn */
#define GRANT_POWER     0x0020u   /* power (reboot/shutdown) */
#define GRANT_ENDPOINT  0x0040u   /* ep_create (may own an IPC endpoint -> broker) */
#define GRANT_IOPORT    0x0080u   /* (reserved v1) raw port I/O / driver tier */
#define GRANT_FRAMEBUFFER 0x0100u /* SYS_FB_OP: draw to the framebuffer (in baseline
                                   for now; split to a display-server tier later) */

/* a broker executive's baseline authority */
/* The ABI (abi/cxk_abi.h) carries a copy of these so userspace spawners can
   request caps. The two MUST agree - guarded here so including both is
   silent, and checked below so a future edit to one can't drift. */
#ifndef GRANT_OS_BASELINE
#define GRANT_OS_BASELINE \
    (GRANT_CONSOLE | GRANT_MEM | GRANT_DISK | GRANT_SPAWN | GRANT_POWER | GRANT_ENDPOINT | GRANT_FRAMEBUFFER | GRANT_NET)
#endif

/* A system program's baseline: what an .xsex gets when the KERNEL starts it.
   Narrower than the executive's on purpose - a system program is OS-owned but
   is not the OS, so it gets enough to say something and to read its own files,
   and asks for anything more by being started with it. Anything launched from
   ring 3 is attenuated against its launcher as usual and never sees this. */
#ifndef GRANT_SYSTEM_BASELINE
#define GRANT_SYSTEM_BASELINE  (GRANT_CONSOLE | GRANT_DISK)
#endif


/* policy layer: map a verified image's identity (CXEX type code + trust) to its
   capability set. Consulted once, at the ring-3 handoff. A valid signature does
   NOT itself grant authority - it only lets us look the identity up here. */
uint32_t caps_for(uint16_t type_code, int trusted);

#endif