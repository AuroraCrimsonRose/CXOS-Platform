// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/boot64.c */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 kernel as roadmap step 5b leaves it: entered in long mode by
   stage 2 (boot/long.asm), it installs its own GDT, TSS and IDT, proves them
   with the bring-up self-tests below, says so on COM1, and halts. The generic
   kernel joins it from step 5c (paging, the page database), and this file's
   job then moves to kmain. Listed in docs/planning/TRANSITIONAL_CODE.md.

   Each test is checked against a sabotage, and some sabotages do not turn a
   test red but stop the boot: with no IST on the double-fault gate the fault
   cannot be delivered, the CPU triple-faults and resets, and there is no
   summary line at all. cxk verify-boot never reads a missing summary as a
   pass. */

#include <stdint.h>
#include "cxk_boot.h"
#include "early_con.h"
#include "gdt.h"
#include "idt.h"
#include "paging_defs.h"
#include "arch/x86/msr.h"

#define MSR_EFER    0xC0000080u
#define EFER_LME    (1ull << 8)
#define EFER_LMA    (1ull << 10)
#define EFER_NXE    (1ull << 11)

#define VEC_BREAKPOINT     3
#define VEC_INVALID_OPCODE 6
#define VEC_DOUBLE_FAULT   8
#define VEC_GP             13

/* A selector one slot past the GDT's end, so loading it is a #GP whose error
   code is the selector itself. */
#define SEL_PAST_GDT       0x28

void boot64_main(uint64_t cxbi_phys);

static uint64_t cxbi_phys_addr;

struct dt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* ---- the probe: what the exception tests expect, and what arrived ---- */
static volatile struct {
    uint64_t vector;            /* the one exception to take */
    uint64_t resume_rip;        /* 0 = resume where the CPU left RIP */
    uint64_t resume_rsp;        /* 0 = leave RSP alone */
    int      hits;
    uint64_t error, rip, frame;
} expect;

static int probe(struct trap_frame64 *f) {
    if (f->vector != expect.vector) return 0;   /* anything else still panics */
    expect.hits++;
    expect.error = f->error;
    expect.rip   = f->rip;
    expect.frame = (uint64_t)(uintptr_t)f;
    if (expect.resume_rip) f->rip = expect.resume_rip;
    if (expect.resume_rsp) f->rsp = expect.resume_rsp;
    return 1;
}

static void arm(uint64_t vector) {
    expect.vector = vector;
    expect.resume_rip = 0;
    expect.resume_rsp = 0;
    expect.hits = 0;
    expect.error = expect.rip = expect.frame = ~0ull;
    idt64_set_probe(probe);
}

static void disarm(void) { idt64_set_probe(0); }

/* ---- the tests: each returns 1 for a pass ---- */

static int t_long_mode(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    return (efer & EFER_LME) && (efer & EFER_LMA);
}

static int t_nx(void) {
    return (rdmsr(MSR_EFER) & EFER_NXE) != 0;
}

static int t_top_2g(void) {
    uintptr_t rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    return (uintptr_t)&boot64_main >= KERNEL_VBASE && rsp >= KERNEL_VBASE;
}

static int t_cxbi(void) {
    if (cxbi_phys_addr == 0 || cxbi_phys_addr >= 0x40000000ull) return 0;
    const struct cxk_boot_info *bi =
        (const struct cxk_boot_info *)(uintptr_t)(KERNEL_VBASE + cxbi_phys_addr);
    return bi->magic == CXBI_MAGIC && bi->version == CXBI_VERSION &&
           bi->size == sizeof(struct cxk_boot_info) &&
           bi->firmware == CXBI_FW_BIOS && bi->mmap_entries > 0;
}

static int t_gdt(void) {
    struct dt_ptr g;
    uint16_t cs, ss;
    __asm__ volatile ("sgdt %0" : "=m"(g));
    __asm__ volatile ("movw %%cs, %0" : "=r"(cs));
    __asm__ volatile ("movw %%ss, %0" : "=r"(ss));
    return g.base == gdt64_base() && g.limit == 5 * 8 - 1 &&
           cs == GDT64_KCODE && ss == GDT64_KDATA;
}

static int t_tss(void) {
    uint16_t tr;
    __asm__ volatile ("str %0" : "=r"(tr));
    /* ltr marks the descriptor busy: type 0x9 becomes 0xB */
    return tr == GDT64_TSS && ((gdt64_tss_descriptor() >> 40) & 0xFF) == 0x8B;
}

static int t_idt(void) {
    struct dt_ptr i;
    __asm__ volatile ("sidt %0" : "=m"(i));
    return i.base == idt64_base() && i.limit == 256 * 16 - 1 &&
           idt64_gate_ist(VEC_DOUBLE_FAULT) == IST_DOUBLE_FAULT &&
           idt64_gate_ist(VEC_GP) == 0;
}

static int t_breakpoint(void) {
    uint64_t after;
    arm(VEC_BREAKPOINT);
    __asm__ volatile ("int3\n"
                      "1: leaq 1b(%%rip), %0" : "=r"(after) : : "memory");
    disarm();
    /* a trap: the CPU reports the instruction after int3 */
    return expect.hits == 1 && expect.rip == after && expect.error == 0;
}

static int t_invalid_opcode(void) {
    uint64_t at;
    arm(VEC_INVALID_OPCODE);
    __asm__ volatile ("leaq 2f(%%rip), %%rax\n\t"
                      "movq %%rax, %1\n\t"
                      "leaq 1f(%%rip), %0\n"
                      "1: ud2\n"
                      "2:"
                      : "=&r"(at), "=m"(expect.resume_rip) : : "rax", "memory");
    disarm();
    /* a fault: the CPU reports the faulting instruction itself */
    return expect.hits == 1 && expect.rip == at && expect.error == 0;
}

static int t_general_protection(void) {
    uint64_t at;
    arm(VEC_GP);
    __asm__ volatile ("leaq 2f(%%rip), %%rax\n\t"
                      "movq %%rax, %1\n\t"
                      "leaq 1f(%%rip), %0\n\t"
                      "movl %2, %%eax\n"
                      "1: movw %%ax, %%ds\n"
                      "2:"
                      : "=&r"(at), "=m"(expect.resume_rip)
                      : "i"(SEL_PAST_GDT) : "rax", "memory");
    disarm();
    uint16_t ds;
    __asm__ volatile ("movw %%ds, %0" : "=r"(ds));
    return expect.hits == 1 && expect.rip == at &&
           expect.error == SEL_PAST_GDT && ds == GDT64_KDATA;
}

/* With RSP non-canonical, int3 cannot push its frame: a stack fault, which
   cannot be pushed either, so a double fault. Only an IST stack can take it;
   without one the CPU shuts down. The probe puts RSP back and resumes after
   the int3. */
static int t_double_fault_ist(void) {
    uintptr_t lo, hi;
    gdt64_df_stack(&lo, &hi);
    arm(VEC_DOUBLE_FAULT);
    __asm__ volatile ("leaq 1f(%%rip), %%rax\n\t"
                      "movq %%rax, %0\n\t"
                      "movq %%rsp, %1\n\t"
                      "movabsq $0x8000000000000000, %%rsp\n\t"
                      "int3\n"
                      "1:"
                      : "=m"(expect.resume_rip), "=m"(expect.resume_rsp)
                      : : "rax", "memory");
    disarm();
    return expect.hits == 1 && expect.error == 0 &&
           expect.frame >= lo && expect.frame < hi;
}

static const struct {
    const char *name;
    int (*fn)(void);
} tests[] = {
    { "long mode is active (EFER.LMA)",                 t_long_mode },
    { "no-execute is enabled (EFER.NXE)",               t_nx },
    { "the kernel and its stack run in the top 2 GiB",  t_top_2g },
    { "the loader handed over CXBI",                    t_cxbi },
    { "the kernel's GDT is loaded",                     t_gdt },
    { "the TSS is loaded and busy",                     t_tss },
    { "the IDT is loaded; #DF has IST 1",               t_idt },
    { "#BP returns through the IDT after int3",         t_breakpoint },
    { "#UD reports the faulting RIP",                   t_invalid_opcode },
    { "#GP carries the selector as its error code",     t_general_protection },
    { "a double fault runs on its IST stack",           t_double_fault_ist },
};

void boot64_main(uint64_t cxbi_phys) {
    cxbi_phys_addr = cxbi_phys;
    early_con_init();
    early_puts("\n");
    early_log("BOOT", EARLY_INFO, "CXK x86-64, roadmap step 5b: long mode, GDT, TSS, IDT");
    early_log_start("BOOT", EARLY_INFO);
    early_puts("CXBI at ");
    early_hex(cxbi_phys);
    early_puts("\n");

    gdt64_init();
    early_log("GDT", EARLY_OK, "GDT + TSS installed (double fault on IST 1)");
    idt64_init();
    early_log("IDT", EARLY_OK, "installed (exceptions 0-31)");

    unsigned n = sizeof(tests) / sizeof(tests[0]), passed = 0;
    for (unsigned i = 0; i < n; i++) {
        int ok = tests[i].fn();
        passed += ok ? 1u : 0u;
        early_log("KTEST", ok ? EARLY_OK : EARLY_FAIL, tests[i].name);
    }

    early_log_start("KTEST", passed == n ? EARLY_OK : EARLY_FAIL);
    if (passed == n) {
        early_puts("self-tests: all ");
        early_dec(n);
        early_puts(" passed\n");
    } else {
        early_puts("self-tests failed: ");
        early_dec(n - passed);
        early_puts(" of ");
        early_dec(n);
        early_puts("\n");
    }
    early_log("BOOT", EARLY_INFO, "nothing more to run until step 5c; halted");
    for (;;) __asm__ volatile ("cli; hlt");
}
