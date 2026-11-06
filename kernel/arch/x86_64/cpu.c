// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/cpu.c */
/* Aurora Tejeda / CATX Systems */
/* arch/cpu.h on x86-64: the GDT, the TSS and its RSP0, the IDT, the feature
   table, and a new thread's first frame; and arch/usermode.h's gate, which
   is syscall/sysret rather than x86-32's int 0x80 (usermode.asm). */

#include "arch/cpu.h"
#include "arch/cpu_ops.h"
#include "arch/usermode.h"
#include "arch/x86/msr.h"
#include "arch/x86/cpufeat.h"
#include "gdt.h"
#include "idt.h"
#include "pat.h"
#include "logging.h"
#include "cpu/percpu.h"
#include "smp.h"

/* The CPU's own data behind GS (cpu/percpu.h), and ring 3's base, which
   swapgs swaps in, 0. Loading a selector into GS zeroes the base, so this
   comes after gdt64_init's segment loads, as entry.asm's came before them. */
static void cpu_local_bind(struct cpu *c) {
    wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void arch_cpu_init(void) {
    gdt64_init();
    cpu_local_bind(&cpu_boot);
    arch_kernel_stack_set(arch_kernel_stack_default());
    klog("GDT", SEV_OK, "GDT + TSS installed (double fault on IST 1)");
    idt64_init();
    klog("IDT", SEV_OK, "installed (exceptions, IRQs 0-15, MSI pool)");
    /* Before anything that might ask what the CPU has. */
    cpufeat_init();
    /* Before any page is mapped write-combining: the framebuffer's (fb.c). */
    pat_init();
    if (pat_wc_ready()) klog("PAT", SEV_OK, "entry 4 is write-combining");
    else                klog("PAT", SEV_WARN, "no page attribute table: the framebuffer stays cached by the MTRRs");
}

/* An AP's, in arch_cpu_init's order (1.9d, smp.c): its own GDT and TSS, its
   struct cpu behind GS, its fallback RSP0, the one IDT; then what each CPU
   holds in its own MSRs and the boot CPU set in its own: the PAT (CX_HW_
   REPORT's "SMP must call pat_init on every AP", 1.8d) and the syscall
   gate. The feature table is the boot CPU's, read once: the rig's eight
   cores are one die. Its CR0 and CR4 came with the trampoline. */
int arch_cpu_init_ap(struct cpu *c, const uintptr_t ist_lo[IST_STACKS], uintptr_t rsp0_top) {
    if (gdt64_init_cpu(c->index, ist_lo, rsp0_top)) return -1;
    cpu_local_bind(c);
    arch_kernel_stack_set(rsp0_top);
    idt64_load();
    pat_init();
    arch_syscall_gate_init();
    return 0;
}

static int pat_ready;

void pat_init(void) {
    if (!cpu_has(CPU_PAT)) return;
    wrmsr(MSR_PAT, PAT_CXK);
    pat_ready = 1;
}

int pat_wc_ready(void) { return pat_ready; }

extern void syscall64_entry(void);   /* usermode.asm */

void arch_syscall_gate_init(void) {
    wrmsr(MSR_STAR, STAR_VALUE);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall64_entry);
    wrmsr(MSR_FMASK, FMASK_VALUE);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void arch_syscall_stub(uint8_t *dst, uint32_t nr) {
    dst[0] = 0xB8;                               /* mov eax, nr */
    dst[1] = (uint8_t)nr;         dst[2] = (uint8_t)(nr >> 8);
    dst[3] = (uint8_t)(nr >> 16); dst[4] = (uint8_t)(nr >> 24);
    dst[5] = 0x0F; dst[6] = 0x05;                /* syscall */
    dst[7] = 0xC3;                               /* ret */
}

uint32_t arch_syscall_stub_ret(void) { return 7; }   /* the ret */

/* Both places the CPU takes it from: the TSS for an interrupt from ring 3,
   the CPU's own data for the syscall entry, which has no hardware switch. */
void arch_kernel_stack_set(uintptr_t top) {
    gdt64_set_rsp0(top);
    cpu_this()->kstack = top;
}

uintptr_t arch_kernel_stack_default(void) { return gdt64_default_rsp0(); }

uintptr_t arch_kernel_stack_current(void) { return gdt64_rsp0(); }

/* FS_BASE by MSR: the FX-8350 has no FSGSBASE. Written on every switch, not
   only when it differs from the last value written, because ring 3 can change
   it behind the kernel's back: loading a selector into FS sets the base from
   the descriptor (XNU's machine_thread_set_tsd_base reads the MSR back for
   the same reason). The kernel itself never uses FS. */
void arch_tls_base_set(uint64_t base) { wrmsr(MSR_FS_BASE, base); }

uint64_t arch_tls_base_current(void) { return rdmsr(MSR_FS_BASE); }

void arch_run_on_stack(uintptr_t top, void (*fn)(void)) {
    /* Fixed registers, as on x86-32: the compiler may hand out rbp, which the
       xor clears before the call reads it. `top` is 16-byte aligned, so fn is
       entered as the ABI expects, 8 below a multiple of 16. */
    __asm__ volatile ("mov %0, %%rsp\n\t"
                      "xor %%ebp, %%ebp\n\t"      /* ends a panic's stack trace here */
                      "call *%1"
                      : : "a"(top), "c"(fn) : "memory");
    __builtin_unreachable();
}

uintptr_t arch_thread_frame(uintptr_t stack_top, void (*launch)(void)) {
    /* context_switch (switch.asm) saves pushfq, rbx, rbp, r12-r15, so from
       the saved RSP upward the frame is
       [r15][r14][r13][r12][rbp][rbx][rflags][return], restored with the
       mirror pops, popfq and ret. Above the return sits a zero where launch's
       own return address would be: launch then starts with RSP 8 below a
       multiple of 16, as after a call, and a stack trace ends there.
       Interrupts off: the thread that switches here holds the scheduler's
       lock, which launch gives back before it turns them on (sched.c). */
    uint64_t *sp = (uint64_t *)(stack_top & ~(uintptr_t)15);
    *(--sp) = 0;                          /* launch's return address: none */
    *(--sp) = (uint64_t)(uintptr_t)launch;/* context_switch's ret target */
    *(--sp) = 0x002;                      /* rflags: IF clear, reserved bit 1 */
    *(--sp) = 0;                          /* rbx */
    *(--sp) = 0;                          /* rbp */
    *(--sp) = 0;                          /* r12 */
    *(--sp) = 0;                          /* r13 */
    *(--sp) = 0;                          /* r14 */
    *(--sp) = 0;                          /* r15  <- RSP points here */
    return (uintptr_t)sp;
}

void arch_cpu_reset(void) {
    /* A triple fault: with an empty IDT the breakpoint cannot be delivered,
       nor can the double fault that follows, and the processor resets. */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile ("cli; lidt %0; int $0x03" : : "m"(null_idt));
    arch_cpu_halt_forever();
}

int arch_cpu_has_random(void) { return cpu_has(CPU_RDSEED) || cpu_has(CPU_RDRAND); }

int arch_cpu_random(uint64_t *v) {
    uint8_t ok = 0;
    if (cpu_has(CPU_RDSEED)) {
        __asm__ volatile ("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok) : : "cc");
        if (ok) return 1;
    }
    if (cpu_has(CPU_RDRAND)) {
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok) : : "cc");
        if (ok) return 1;
    }
    return 0;
}
