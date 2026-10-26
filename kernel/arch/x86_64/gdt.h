// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/gdt.h */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 GDT and TSS. The user segments sit where syscall and sysret
   look for them (usermode.asm): syscall takes CS and SS from STAR[47:32] and
   the slot after it, sysretq takes SS from STAR[63:48] + 8 and CS from
   STAR[63:48] + 16. With STAR[63:48] = GDT64_KDATA that is user data, then
   user code, right after the kernel's. */

#ifndef ARCH_X86_64_GDT_H
#define ARCH_X86_64_GDT_H

#include <stdint.h>

#define GDT64_KCODE   0x08
#define GDT64_KDATA   0x10
#define GDT64_UDATA   0x18      /* loaded as 0x1B, RPL 3 */
#define GDT64_UCODE   0x20      /* loaded as 0x23, RPL 3 */
#define GDT64_TSS     0x28      /* a 16-byte descriptor: slots 5 and 6 */

#define USER_DATA_SEL (GDT64_UDATA | 3)
#define USER_CODE_SEL (GDT64_UCODE | 3)

/* The MSRs syscall and sysret read (cpu.c sets them, arch_syscall_gate_init).
   STAR[47:32] is the kernel CS syscall loads, STAR[63:48] the base sysretq
   adds 8 (SS) and 16 (CS) to. That base carries RPL 3 itself (0x13): Intel's
   sysret ORs 3 into SS, AMD's loads STAR[63:48] + 8 as it is, so a base of
   0x10 gave ring 3 SS 0x18 on the rig, and the first interrupt's iretq back
   to ring 3 faulted on it (#GP 0x18). Linux's base is __USER32_CS, RPL 3 for
   the same reason. FMASK names the RFLAGS bits syscall clears:
   IF, so the entry runs with interrupts off until it is on RSP0; TF, DF, AC
   and NT, so ring 3 cannot hand the kernel single-stepping, a backward
   string direction, unchecked user access or a nested task; and IOPL. */
#define MSR_EFER      0xC0000080u
#define MSR_STAR      0xC0000081u
#define MSR_LSTAR     0xC0000082u
#define MSR_FMASK     0xC0000084u
#define EFER_SCE      (1ull << 0)
#define STAR_VALUE    (((uint64_t)(USER_DATA_SEL - 8) << 48) | ((uint64_t)GDT64_KCODE << 32))
#define FMASK_VALUE   0x47700ull     /* TF IF DF IOPL NT AC */

/* The IST slots. The double fault's is the guarantee the 32-bit kernel's task
   gate gives, a good stack whatever state RSP was in when the fault hit. NMI,
   #DB and #MC have their own for the same reason: syscall runs its first
   instructions in ring 0 on the user's RSP, and an NMI there would otherwise
   push its frame wherever ring 3 pointed RSP (usermode.asm). */
#define IST_DOUBLE_FAULT  1
#define IST_NMI           2
#define IST_DEBUG         3
#define IST_MACHINE_CHECK 4

/* Where the TSS keeps RSP0, for the syscall entry (usermode.asm). */
#define TSS64_RSP0_OFFSET 4

void gdt64_init(void);

/* For the bring-up self-tests: where the GDT is, descriptor n, the TSS
   descriptor's low quadword (its type says whether the TSS is busy, so whether
   ltr ran), and an IST stack's bounds. */
uintptr_t gdt64_base(void);
uint64_t  gdt64_descriptor(unsigned n);
uint64_t  gdt64_tss_descriptor(void);
void      gdt64_ist_stack(unsigned ist, uintptr_t *lo, uintptr_t *hi);

/* RSP0, the stack an interrupt from ring 3 switches to and the syscall entry
   moves to (cpu.c sets it on every thread switch), and the dedicated one used
   while the running thread has none. */
void      gdt64_set_rsp0(uintptr_t top);
uintptr_t gdt64_rsp0(void);
uintptr_t gdt64_default_rsp0(void);

#endif
