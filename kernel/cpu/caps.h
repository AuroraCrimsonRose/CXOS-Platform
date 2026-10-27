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
 *     if (!(thread_current_caps() & GRANT_X)) return E_ACCESS_DENIED;
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

/* The grants, one bit each, are ABI: their values are in abi/kernel.xfil, and
   so in cxk_abi.h, where userspace spawners name them too. A process holds some
   subset; a launcher passes a subset of its own to a child and can never
   amplify (see the attenuation note in docs/x86_32/kernel/CX_ABI.md section 5).
   What each one guards in the kernel:

     GRANT_CONSOLE      console_write
     GRANT_MEM          (reserved, not enforced) see the note below
     GRANT_DISK         block_read / block_write
     GRANT_NET          SYS_NET_*: interface config, ping, raw frames
     GRANT_SPAWN        spawn
     GRANT_POWER        power (reboot/shutdown)
     GRANT_ENDPOINT     ep_create (may own an IPC endpoint -> broker)
     GRANT_IOPORT       (reserved v1) raw port I/O / driver tier
     GRANT_FRAMEBUFFER  SYS_FB_*: draw to the framebuffer (in baseline for
                        now; split to a display-server tier later)
     GRANT_OS_BASELINE  a broker executive's baseline authority

   GRANT_MEM is NOT ENFORCED, and that is deliberate rather than missing. The
   SYS_VM_* calls are unprivileged by design - "a program that could not obtain memory would not
   be contained, it would be unable to run", and what bounds it is the
   per-process quota inside vm_map, not a bit every program would have to hold
   (see the comment on syscall_vm_map in cpu/usermode.c). This bit is therefore
   vestigial: it is carried in GRANT_OS_BASELINE and accepted by the
   supervisor's descriptor parser, and it gates nothing.

   Found by the syscall-authorisation audit of 2026-10-09, which came looking
   for a missing check and found a misleading definition instead - this said
   "map / unmap / sbrk" as though it controlled them. Kept rather than removed,
   because removing a grant bit is an ABI change and the value is already
   published; marked reserved, the way GRANT_IOPORT already is. If memory ever
   does become a gated resource, this is the bit to use and the comment on
   syscall_vm_map is the decision to revisit first. */

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