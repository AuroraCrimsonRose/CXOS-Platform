/* /CXLite/kernel/cpu/fpu.c */
/* Aurora Tejeda */
/*
 * FPU + SSE bring-up.
 *
 * ---- STATUS: NOT IN THE BUILD, AND DELIBERATELY SO ----------------------
 *
 * This file is not listed in CMakeLists and fpu_init() is called from nowhere.
 * That is currently correct, and there are two separate reasons:
 *
 *  1. Nothing needs it. kmath.c (the only floating-point code in the tree) is
 *     also unbuilt and nothing calls km_*. The kernel executes no FP at all.
 *     x87 would in fact work untouched if it did: CR0.EM and CR0.TS are both 0
 *     after reset, so x87 is live from power-on. Only SSE actually requires the
 *     CR4 bits set below.
 *
 *  2. Turning it on today would introduce a bug. The comment this file used to
 *     carry - "single-threaded for now, so no per-task FPU save/restore" - has
 *     been false since preemptible ring-3 processes landed. context_switch
 *     saves no FPU or SSE state, so the moment two processes both used FP their
 *     registers would silently corrupt each other. Enabling FP is blocked on
 *     doing that properly: either fxsave/fxrstor in the switch path, or lazy
 *     switching via CR0.TS and the #7 (Device Not Available) handler.
 *
 * So this is kept as the correct implementation for when FP is wanted, not as
 * something to switch on. Before wiring it in, do the save/restore work first.
 *
 * ---- What it does ------------------------------------------------------
 *
 *   CR0.EM (bit 2) = 0   - "emulation" off; let FPU/SSE execute natively
 *                          (if 1, FP instructions trap as #7 Device Not Avail)
 *   CR0.MP (bit 1) = 1   - "monitor coprocessor"
 *   CR0.TS (bit 3) = 0   - "task switched" off (no lazy-FP trap)
 *   CR4.OSFXSR    (bit 9)  = 1 - OS supports fxsave/fxrstor; enables SSE
 *   CR4.OSXMMEXCPT(bit 10) = 1 - unmasked SSE FP exceptions reported as #19
 * then `fninit` for a known x87 state.
 *
 * Each of those is now conditional on the CPU actually having the feature.
 * Setting CR4.OSFXSR on a processor without SSE raises #GP, and in a kernel
 * this early that is a triple fault and an instant reboot loop with nothing on
 * screen to say why. "i686" includes the Pentium Pro and Pentium II, which have
 * x87 but no SSE, so this is not a theoretical concern on old hardware.
 */

#include "fpu.h"
#include <stdint.h>

/* CPUID leaf 1, EDX feature bits */
#define CPUID_EDX_FPU   (1u << 0)
#define CPUID_EDX_FXSR  (1u << 24)
#define CPUID_EDX_SSE   (1u << 25)

static int have_sse = 0;
static int have_x87 = 0;

/* CPUID is guaranteed on i686 (Pentium Pro and later), which is the minimum
   this kernel targets, so the EFLAGS.ID availability dance is not needed.
   EBX is saved and restored by hand rather than constrained, so this compiles
   the same whether or not the build is position-independent. */
static void cpuid1(uint32_t *ecx_out, uint32_t *edx_out) {
    uint32_t ebx_val, ecx_val, edx_val;
    __asm__ volatile (
        "pushl %%ebx      \n\t"
        "cpuid            \n\t"
        "movl  %%ebx, %0  \n\t"
        "popl  %%ebx      \n\t"
        : "=r"(ebx_val), "=c"(ecx_val), "=d"(edx_val)
        : "a"(1u)
        : "cc");
    (void)ebx_val;
    *ecx_out = ecx_val;
    *edx_out = edx_val;
}

void fpu_init(void) {
    uint32_t ecx, edx;
    cpuid1(&ecx, &edx);
    (void)ecx;

    have_x87 = (edx & CPUID_EDX_FPU)  ? 1 : 0;
    /* SSE needs BOTH the instructions and fxsave/fxrstor: OSFXSR advertises
       fxsave support, so setting it without FXSR would be a lie even where it
       does not fault. */
    have_sse = ((edx & CPUID_EDX_SSE) && (edx & CPUID_EDX_FXSR)) ? 1 : 0;

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    if (have_x87 || have_sse) {
        cr0 &= ~(1u << 2);     /* clear EM  - execute FP natively */
        cr0 |=  (1u << 1);     /* set   MP  */
        cr0 &= ~(1u << 3);     /* clear TS  */
    } else {
        /* No FPU at all: leave EM set so an FP instruction traps as #7 rather
           than executing as garbage. There is no emulator, but a clean fault is
           a far better failure than silent nonsense. */
        cr0 |= (1u << 2);
    }
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    if (have_sse) {
        uint32_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1u << 9);      /* OSFXSR     */
        cr4 |= (1u << 10);     /* OSXMMEXCPT */
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
    }

    if (have_x87) __asm__ volatile ("fninit");
}

int fpu_has_x87(void) { return have_x87; }
int fpu_has_sse(void) { return have_sse; }
