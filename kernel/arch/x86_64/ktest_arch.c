// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/ktest_arch.c */
/* Aurora Tejeda / CATX Systems */
/* arch/ktest_arch.h on x86-64: the boot self-tests that read the CPU's own
   state - its tables, its MSRs, its page tables, its exceptions and ring 3 -
   run by ktest_run with the rest, on thread 0 after kmain has brought up
   everything. They were boot64.c's, the x86-64 kernel's whole run from step
   5b to 5e, written as each piece arrived; kmain took that job over at step
   5f1.

   Each test is checked against a sabotage, and some sabotages do not turn a
   test red but stop the boot: with no IST on the double-fault gate the fault
   cannot be delivered, the CPU triple-faults and resets, and there is no
   summary line at all. cxk verify-boot never reads a missing summary as a
   pass. */

#include <stdint.h>
#include "cxk_boot.h"
#include "gdt.h"
#include "idt.h"
#include "pagetable.h"
#include "pat.h"
#include "fb.h"                 /* fb_test_screen_virt: the screen is VM_WC */
#include "arch/x86/msr.h"
#include "boot/cxbi.h"
#include "memman/pagedb.h"
#include "memman/ocache.h"
#include "pmm.h"
#include "heap.h"
#include "kstack.h"
#include "sched.h"
#include "timer.h"
#include "acpi.h"
#include "logging.h"
#include "panic.h"
#include "arch/cpu.h"
#include "arch/cpu_ops.h"
#include "arch/x86/apic.h"
#include "platform/pc/pic.h"
#include "proc.h"
#include "caps.h"
#include "sysrec.h"
#include "usermode.h"
#include "diag/ktest_umem.h"   /* ktest_umem: a test thread's own user pages */
#include "syspage.h"            /* SYSPAGE_VIRT, syspage_map */
#include "arch/ktest_arch.h"
#include "arch/usermode.h"
#include "arch/evtimer.h"        /* arch_cycles_constant */
#include "arch/x86/cpufeat.h"    /* cpuid_read: Hyper-V's grant, read here */
#include "cpu/percpu.h"
#include "smp.h"                /* smp_stacks_of, smp_timer_hits */
#include "arch/smp.h"           /* smp_call, the shootdown */
#include "sync/spinlock.h"      /* a lock spun on while a shootdown waits */
#include "bignum.h"             /* bn_modexp: a long interrupts-off computation */
#include "rsa.h"                /* RSA_PROFILE_EXPONENT */
#include "arch/x86/madt.h"
#include "object/ktimer.h"
#include "object/kwait.h"
#include "cxk_abi.h"            /* SIGNAL_TIMER_FIRED, E_TIMED_OUT */
#include "disk.h"               /* the storage lock (1.9e3) */
#include "console.h"            /* the log lock */
#include "cxfs.h"
#include "syslog.h"
#include "sysfile.h"            /* files_lock */
#include "job.h"                /* jobs_lock (1.9e4) */
#include "keyboard.h"           /* the input buffer's lock (1.9e4) */
#include "platform/platform.h"  /* platform_wallclock: the CMOS lock */
#include "format.h"
#include "string.h"

#define EFER_LME    (1ull << 8)
#define EFER_LMA    (1ull << 10)
#define EFER_NXE    (1ull << 11)

#define VEC_DEBUG          1
#define VEC_NMI            2
#define VEC_BREAKPOINT     3
#define VEC_INVALID_OPCODE 6
#define VEC_DOUBLE_FAULT   8
#define VEC_GP             13
#define VEC_PAGE_FAULT     14
#define VEC_MACHINE_CHECK  18

/* the #PF error code */
#define PF_PRESENT         0x01u   /* a protection fault, not a missing page */
#define PF_WRITE           0x02u
#define PF_USER            0x04u   /* taken in ring 3 */
#define PF_FETCH           0x10u   /* an instruction fetch: no-execute */

/* A selector one slot past the GDT's end, so loading it is a #GP whose error
   code is the selector itself. */
#define SEL_PAST_GDT       0x38

#define LOADER_PML4        0x3000u        /* boot/long.asm's PT_PML4 */
#define FOUR_GB            0x100000000ull
#define LAPIC_DEFAULT      0xFEE00000ull  /* the local APIC's registers, where firmware leaves them */

/* A page the tests map and unmap, in a region of its own (paging_defs.h). */
#define SCRATCH_VA         KVA_KTEST_SCRATCH

void kmain(uintptr_t cxbi_phys);

extern uint8_t __kernel_end[];   /* the linker's, virtual */

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
    uint64_t gs;                /* GS_BASE as the handler ran: the kernel's, swapped in */
} expect;

static int probe(struct registers *f) {
    if (f->vector != expect.vector) return 0;   /* anything else still panics */
    expect.hits++;
    expect.error = f->error;
    expect.rip   = f->rip;
    expect.frame = (uint64_t)(uintptr_t)f;
    expect.gs    = rdmsr(MSR_GS_BASE);
    if (expect.resume_rip) f->rip = expect.resume_rip;
    if (expect.resume_rsp) f->rsp = expect.resume_rsp;
    return 1;
}

static void arm(uint64_t vector) {
    expect.vector = vector;
    expect.resume_rip = 0;
    expect.resume_rsp = 0;
    expect.hits = 0;
    expect.error = expect.rip = expect.frame = expect.gs = ~0ull;
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

/* The image, entry to end. Thread 0's stack is a guarded one by now, in the
   stacks' region (t_main_stack_guarded). */
static int t_top_2g(void) {
    return (uintptr_t)&kmain >= KERNEL_VBASE && (uintptr_t)__kernel_end > (uintptr_t)&kmain;
}

/* The kernel's copy (boot/cxbi.c), taken while the loader's mapping of low
   memory was still there; cxbi_init refuses a handover outside it. */
static int t_cxbi(void) {
    const struct cxk_boot_info *bi = cxbi();
    return bi->magic == CXBI_MAGIC && bi->version == CXBI_VERSION &&
           bi->size == sizeof(struct cxk_boot_info) &&
           bi->firmware == CXBI_FW_BIOS && bi->mmap_entries > 0 &&
           cxbi_mmap_count() > 0;
}

static int t_gdt(void) {
    struct dt_ptr g;
    uint16_t cs, ss;
    __asm__ volatile ("sgdt %0" : "=m"(g));
    __asm__ volatile ("movw %%cs, %0" : "=r"(cs));
    __asm__ volatile ("movw %%ss, %0" : "=r"(ss));
    return g.base == gdt64_base() && g.limit == 7 * 8 - 1 &&
           cs == GDT64_KCODE && ss == GDT64_KDATA &&
           ((gdt64_descriptor(GDT64_UCODE / 8) >> 40) & 0xFF) == 0xFA &&   /* ring 3 code */
           ((gdt64_descriptor(GDT64_UCODE / 8) >> 53) & 1) == 1 &&         /* 64-bit */
           ((gdt64_descriptor(GDT64_UDATA / 8) >> 40) & 0xFF) == 0xF2;     /* ring 3 data */
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
           idt64_gate_ist(VEC_NMI) == IST_NMI &&
           idt64_gate_ist(VEC_DEBUG) == IST_DEBUG &&
           idt64_gate_ist(VEC_MACHINE_CHECK) == IST_MACHINE_CHECK &&
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
    gdt64_ist_stack(IST_DOUBLE_FAULT, &lo, &hi);
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

/* ---- paging (step 5c) ---- */

static uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static uint64_t up4k(uint64_t x)   { return (x + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1); }
static uint64_t down4k(uint64_t x) { return x & ~(uint64_t)(PAGE_SIZE - 1); }

/* Touch `addr` once - read a byte, write the same byte back, or call it - and
   say whether that raised a page fault; expect.error and CR2 then tell what
   kind. A touch that does not fault does no harm: the write puts back what was
   there, and a call runs the `ret` the caller left at `addr`. */
static int faults_on_read(uint64_t addr) {
    arm(VEC_PAGE_FAULT);
    __asm__ volatile ("leaq 1f(%%rip), %%rax\n\t"
                      "movq %%rax, %0\n\t"
                      "movb (%1), %%cl\n"
                      "1:"
                      : "=m"(expect.resume_rip) : "r"(addr) : "rax", "rcx", "memory");
    disarm();
    return expect.hits == 1;
}

static int faults_on_write(uint64_t addr) {
    uint8_t was = *(volatile uint8_t *)(uintptr_t)addr;
    arm(VEC_PAGE_FAULT);
    __asm__ volatile ("leaq 1f(%%rip), %%rax\n\t"
                      "movq %%rax, %0\n\t"
                      "movb %2, (%1)\n"
                      "1:"
                      : "=m"(expect.resume_rip) : "r"(addr), "q"(was) : "rax", "memory");
    disarm();
    return expect.hits == 1;
}

static int faults_on_call(uint64_t addr) {
    arm(VEC_PAGE_FAULT);
    __asm__ volatile ("leaq 1f(%%rip), %%rax\n\t"
                      "movq %%rax, %0\n\t"
                      "movq %%rsp, %1\n\t"
                      "call *%2\n"
                      "1:"
                      : "=m"(expect.resume_rip), "=m"(expect.resume_rsp)
                      : "r"(addr) : "rax", "memory");
    disarm();
    return expect.hits == 1;
}

/* the fault was the kind asked about, at `addr` */
static int fault_was(uint64_t addr, uint64_t bits) {
    return read_cr2() == addr && (expect.error & bits) == bits;
}

/* Targets in each section. Nothing calls text_target; its first byte is
   rewritten with itself if the write test fails. */
__attribute__((noinline, used)) static void text_target(void) { __asm__ volatile (""); }
static const uint8_t rodata_target[64] = { 0x5A, 0xA5 };
static uint8_t data_ret[64] = { 0xC3 };   /* ret */

static int is_ram(uint32_t type) {
    return type == CXBI_MEM_USABLE || type == CXBI_MEM_ACPI_RECLAIM ||
           type == CXBI_MEM_ACPI_NVS || type == CXBI_MEM_LOADER;
}

/* every page in [lo, hi) is wholly inside one RAM entry of the map */
static int ram_holds(uint64_t lo, uint64_t hi) {
    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (is_ram(e->type) && e->base <= lo && hi <= e->base + e->length) return 1;
    }
    return 0;
}

static int usable_holds(uint64_t lo, uint64_t hi) {
    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (e->type == CXBI_MEM_USABLE && e->base <= lo && hi <= e->base + e->length) return 1;
    }
    return 0;
}

static int t_own_tables(void) {
    return addr_space_current() == addr_space_kernel() &&
           addr_space_current() != LOADER_PML4 && paging_kernel_obeys_readonly();
}

/* Frame `p` in the direct map. These tests are of x86-64's own tables and
   layout, so they read it directly; code outside the architecture goes
   through memman/physmap.h. */
static void *direct(paddr_t p) { return (void *)(uintptr_t)(DIRECT_MAP_BASE + p); }

/* All 256 kernel entries exist from boot, kernel-only; the user half is
   empty. */
static int t_kernel_half(void) {
    const uint64_t *pml4 = direct(addr_space_kernel());
    for (unsigned i = 0; i < PML4_KERNEL_LO; i++)
        if (pml4[i]) return 0;
    for (unsigned i = PML4_KERNEL_LO; i < PT64_ENTRIES; i++)
        if (!(pml4[i] & PTE64_PRESENT) || (pml4[i] & PTE64_USER)) return 0;
    return 1;
}

/* The loader mapped the first GiB at 0 and at KERNEL_VBASE; neither is left
   but the image itself, and a null pointer faults as a missing page. */
static int t_loader_maps_gone(void) {
    if (paging_get_phys(0x100000) || paging_get_phys(KERNEL_VBASE)) return 0;
    return faults_on_read(0) && fault_was(0, 0) && !(expect.error & PF_PRESENT);
}

static int t_text_readonly(void) {
    uint64_t a = (uint64_t)(uintptr_t)&text_target;
    return faults_on_write(a) && fault_was(a, PF_PRESENT | PF_WRITE);
}

static int t_rodata_readonly(void) {
    uint64_t a = (uint64_t)(uintptr_t)&rodata_target[1];
    return faults_on_write(a) && fault_was(a, PF_PRESENT | PF_WRITE) &&
           rodata_target[1] == 0xA5;
}

static int t_data_stack_nx(void) {
    volatile uint8_t on_stack[16] = { 0xC3 };
    uint64_t d = (uint64_t)(uintptr_t)data_ret, s = (uint64_t)(uintptr_t)on_stack;
    if (!faults_on_call(d) || !fault_was(d, PF_PRESENT | PF_FETCH)) return 0;
    return faults_on_call(s) && fault_was(s, PF_PRESENT | PF_FETCH);
}

/* Each RAM entry of the map, first page and last, is at DIRECT_MAP_BASE plus
   its address; the first page of everything else is not mapped at all,
   unless RAM shares it. */
static int t_direct_map_covers_ram(void) {
    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        uint64_t lo = up4k(e->base), hi = down4k(e->base + e->length);
        if (lo >= hi) continue;
        if (is_ram(e->type)) {
            if (paging_get_phys(DIRECT_MAP_BASE + lo) != lo) return 0;
            if (paging_get_phys(DIRECT_MAP_BASE + hi - PAGE_SIZE) != hi - PAGE_SIZE) return 0;
        } else if (!ram_holds(lo, lo + PAGE_SIZE) && paging_get_phys(DIRECT_MAP_BASE + lo)) {
            return 0;
        }
    }
    /* the local APIC's registers are not RAM */
    return paging_get_phys(DIRECT_MAP_BASE + LAPIC_DEFAULT) == 0;
}

/* A frame mapped somewhere else shows the same bytes in the direct map. */
static int t_direct_map_is_physical(void) {
    paddr_t p = pmm_alloc();
    if (!p) return 0;
    paging_map_kernel(SCRATCH_VA, p, VM_WRITE);
    volatile uint64_t *via_map = (volatile uint64_t *)(uintptr_t)SCRATCH_VA;
    volatile uint64_t *via_direct = (volatile uint64_t *)(uintptr_t)(DIRECT_MAP_BASE + p);
    via_map[0] = 0x5C5C0000DEADBEEFull;
    via_map[511] = p;
    int ok = via_direct[0] == 0x5C5C0000DEADBEEFull && via_direct[511] == p &&
             paging_get_phys(SCRATCH_VA + 8) == p + 8;
    paging_unmap(SCRATCH_VA);
    ok = ok && paging_get_phys(SCRATCH_VA) == 0;
    pmm_free(p);
    return ok;
}

/* Every leaf of the direct map, 2 MiB or 4 KiB, is no-execute and
   kernel-only; and a call into it faults as a fetch from a present page. */
static int direct_map_leaf_ok(uint64_t e) {
    return (e & (PTE64_NX | PTE64_USER)) == PTE64_NX;
}

static int t_direct_map_nx(void) {
    const uint64_t *pml4 = direct(addr_space_kernel());
    unsigned leaves = 0;
    for (unsigned i = PML4_INDEX(DIRECT_MAP_BASE);
         i <= PML4_INDEX(DIRECT_MAP_BASE + DIRECT_MAP_SIZE - 1); i++) {
        if (!(pml4[i] & PTE64_PRESENT)) continue;
        const uint64_t *pdpt = direct(pml4[i] & PTE64_ADDR);
        for (unsigned j = 0; j < PT64_ENTRIES; j++) {
            if (!(pdpt[j] & PTE64_PRESENT)) continue;
            if (pdpt[j] & PTE64_LARGE) return 0;        /* none are made */
            const uint64_t *pd = direct(pdpt[j] & PTE64_ADDR);
            for (unsigned k = 0; k < PT64_ENTRIES; k++) {
                if (!(pd[k] & PTE64_PRESENT)) continue;
                if (pd[k] & PTE64_LARGE) {
                    if (!direct_map_leaf_ok(pd[k])) return 0;
                    leaves++;
                    continue;
                }
                const uint64_t *pt = direct(pd[k] & PTE64_ADDR);
                for (unsigned m = 0; m < PT64_ENTRIES; m++) {
                    if (!(pt[m] & PTE64_PRESENT)) continue;
                    if (!direct_map_leaf_ok(pt[m])) return 0;
                    leaves++;
                }
            }
        }
    }
    if (!leaves) return 0;

    paddr_t p = pmm_alloc();
    if (!p) return 0;
    uint64_t a = DIRECT_MAP_BASE + p;
    *(volatile uint8_t *)(uintptr_t)a = 0xC3;   /* ret */
    int ok = faults_on_call(a) && fault_was(a, PF_PRESENT | PF_FETCH);
    pmm_free(p);
    return ok;
}

/* Pages handed out are usable RAM, above the first MiB, and not the image,
   the boot page tables or the database; all different; and all come back. */
static int t_pmm_hands_out_only_ram(void) {
    paddr_t db, boot;
    uint64_t db_len, boot_len;
    pagedb_database(&db, &db_len);
    pagedb_boot_tables(&boot, &boot_len);
    if (!pmm_total_pages() || !db_len || !boot_len || !usable_holds(db, db + db_len)) return 0;

    enum { N = 64 };
    paddr_t got[N];
    uint32_t before = pmm_free_count();
    int ok = 1;
    for (unsigned i = 0; i < N; i++) {
        /* low and high zone alike */
        paddr_t p = (i & 1) ? pmm_alloc() : pmm_alloc_pages_low(1);
        got[i] = p;
        if (!p || !usable_holds(p, p + PAGE_SIZE) || p < 0x100000 ||
            (p >= db && p < db + db_len) || (p >= boot && p < boot + boot_len) ||
            p < up4k((uint64_t)(uintptr_t)__kernel_end - KERNEL_VBASE)) ok = 0;
        for (unsigned j = 0; j < i; j++) if (got[j] == p) ok = 0;
    }
    if (pmm_free_count() != before - N) ok = 0;
    for (unsigned i = 0; i < N; i++) if (got[i]) pmm_free(got[i]);
    return ok && pmm_free_count() == before;
}

/* Freeing a free page, or a page the allocator never owned, is refused and
   changes nothing (pmm_free would panic). */
static int t_pmm_double_free(void) {
    paddr_t p = pmm_alloc();
    if (!p) return 0;
    uint32_t before = pmm_free_count();
    int ok = pmm_free_checked(p, 1) == 0 && pmm_free_count() == before + 1;
    ok = ok && pmm_free_checked(p, 1) == -1 && pmm_free_count() == before + 1;
    ok = ok && pmm_free_checked((uint64_t)(uintptr_t)&text_target - KERNEL_VBASE, 1) == -1;
    ok = ok && pmm_free_checked(0, 1) == -1;
    return ok && pmm_free_count() == before + 1;
}

/* A run of five is contiguous, starts on its block's boundary (eight pages),
   and costs exactly five: the other three go back. */
static int t_pmm_runs(void) {
    uint32_t before = pmm_free_count();
    paddr_t a = pmm_alloc_pages(5);
    if (!a) return 0;
    int ok = (a & (8 * PAGE_SIZE - 1)) == 0 && pmm_free_count() == before - 5;
    ok = ok && pmm_free_checked(a + 5 * PAGE_SIZE, 1) == -1;   /* given back: free */
    ok = ok && pmm_free_checked(a, 5) == 0;
    return ok && pmm_free_count() == before;
}

/* An ordinary page comes from above 4 GB when there is RAM there; one for a
   32-bit device from below. */
static int t_pmm_zones(void) {
    int high_ram = 0;
    for (uint32_t i = 0; i < cxbi_mmap_count(); i++) {
        const struct cxk_mmap_entry *e = cxbi_mmap(i);
        if (e->type == CXBI_MEM_USABLE && e->base + e->length > FOUR_GB) high_ram = 1;
    }
    paddr_t p = pmm_alloc(), q = pmm_alloc_pages_low(16);
    int ok = p && q && q + 16 * PAGE_SIZE <= FOUR_GB && (!high_ram || p >= FOUR_GB);
    if (p) pmm_free(p);
    if (q) pmm_free_pages(q, 16);
    return ok;
}

static int t_user_map_refuses_kernel(void) {
    return paging_map_user(USER_VLIMIT, 0x200000, 0) == -1 &&
           paging_map_user(DIRECT_MAP_BASE, 0x200000, VM_WRITE) == -1 &&
           paging_map_user(KERNEL_VBASE, 0x200000, VM_WRITE) == -1 &&
           paging_map_user(0x400000, 0x200000, 0x80u) == -1;
}

/* A new space has the kernel half and an empty user half; its user pages are
   ring-3 pages with the rights asked for, absent from the kernel's space;
   releasing it gives back every page it took. */
static int t_address_space(void) {
    uint32_t before = pmm_free_count();
    paddr_t root;
    if (addr_space_create(&root) != 0) return 0;
    const uint64_t *mine = direct(root);
    const uint64_t *kern = direct(addr_space_kernel());
    int ok = 1;
    for (unsigned i = 0; i < PT64_ENTRIES; i++)
        if (mine[i] != (i < PML4_KERNEL_LO ? 0 : kern[i])) ok = 0;

    addr_space_switch(root);
    paddr_t rw = pmm_alloc(), ro = pmm_alloc();
    /* each mapping is checked before the next, which shares its tables */
    ok = ok && addr_space_current() == root && rw && ro &&
         paging_map_user(0x400000, rw, VM_WRITE) == 0 &&
         paging_is_user(0x400000) && paging_is_user_writable(0x400000) &&
         paging_map_user(0x401000, ro, 0) == 0 &&
         paging_is_user(0x401000) && !paging_is_user_writable(0x401000) &&
         !paging_is_user((uintptr_t)&expect) && !paging_is_user(DIRECT_MAP_BASE + rw);
    if (ok) {
        *(volatile uint32_t *)0x400000 = 0xC0FFEEu;   /* SMAP is not on yet */
        ok = *(volatile uint32_t *)(uintptr_t)(DIRECT_MAP_BASE + rw) == 0xC0FFEEu;
    }
    addr_space_switch(addr_space_kernel());
    ok = ok && paging_get_phys(0x400000) == 0;
    addr_space_release(root);   /* frees rw and ro with it */
    return ok && pmm_free_count() == before;
}

/* The 4 KiB entry mapping `virt` in the active space, read through the
   direct map; 0 if there is none. */
static uint64_t entry_of(uint64_t virt) {
    const uint64_t *t = direct(addr_space_current());
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        uint64_t e = t[(virt >> shift) & 511u];
        if (!(e & PTE64_PRESENT) || (e & PTE64_LARGE)) return 0;
        t = direct(e & PTE64_ADDR);
    }
    return t[PT_INDEX(virt)];
}

/* Device registers: the same frame at the same place however it is asked
   for, uncached and no-execute, and it reaches the device - the local APIC's
   version register. */
static int t_map_phys(void) {
    volatile uint8_t *a = paging_map_phys(LAPIC_DEFAULT, 0x400, VM_WRITE | VM_UNCACHED);
    volatile uint8_t *b = paging_map_phys(LAPIC_DEFAULT + 0x30, 4, VM_WRITE | VM_UNCACHED);
    if (!a || b != a + 0x30 || (uintptr_t)a != PHYS_WINDOW_BASE + LAPIC_DEFAULT) return 0;
    uint64_t e = entry_of((uintptr_t)a);
    if ((e & (PTE64_PCD | PTE64_PWT | PTE64_NX | PTE64_USER)) != (PTE64_PCD | PTE64_PWT | PTE64_NX))
        return 0;
    uint32_t version = *(volatile uint32_t *)b & 0xFFu;
    return version >= 0x10 && version <= 0x15 &&
           paging_map_phys(PHYS_WINDOW_SIZE, 1, 0) == 0;
}

/* ---- 1.8d: write-combining for the framebuffer (pat.h) ---- */

/* Entry 4 is write-combining and entries 0-3 are as the CPU starts them, so
   PCD and PWT still mean what VM_UNCACHED needs (entry 3 uncached). */
static int t_pat(void) {
    if (!cpu_has(CPU_PAT)) return !pat_wc_ready();
    uint64_t pat = rdmsr(MSR_PAT);
    return pat_wc_ready() && pat == 0x0007040100070406ull;   /* PAT_CXK, spelled out */
}

/* A VM_WC page is entry 4 (the PAT bit, PCD and PWT clear) and holds what is
   written through it once fenced; and the screen is mapped so (fb.c). */
static int t_wc_pages(void) {
    if (!pat_wc_ready()) return 1;
    paddr_t p = pmm_alloc();
    if (!p) return 0;
    paging_map_kernel(SCRATCH_VA, p, VM_WRITE | VM_WC);
    uint64_t e = entry_of(SCRATCH_VA);
    int ok = (e & (PTE64_PAT | PTE64_PCD | PTE64_PWT)) == PTE64_PAT;
    *(volatile uint64_t *)(uintptr_t)SCRATCH_VA = 0x5743574357435743ull;
    __asm__ volatile ("sfence" ::: "memory");
    ok = ok && *(volatile uint64_t *)(uintptr_t)(DIRECT_MAP_BASE + p) == 0x5743574357435743ull;
    paging_unmap(SCRATCH_VA);
    pmm_free(p);
    if (fb_active()) {
        e = entry_of(fb_test_screen_virt());
        ok = ok && (e & (PTE64_PAT | PTE64_PCD | PTE64_PWT)) == PTE64_PAT;
    }
    return ok;
}

/* ---- step 5d: the heap, interrupts, the timer, threads ---- */

/* The top of the stack this runs on: thread 0's, which kmain took from
   kstack_alloc, at the top of its slot. */
static uintptr_t main_stack_top(void) {
    uintptr_t rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    return (rsp & ~(uintptr_t)(KSTACK_SLOT_SIZE - 1)) + KSTACK_SLOT_SIZE;
}

/* The heap's own round trip: small sizes from the object caches, large ones
   from the list, and every byte back afterwards. */
static int t_heap(void) {
    uint32_t used0 = heap_bytes_used();
    uint8_t *small = kmalloc(64), *big = kmalloc(8192);
    int ok = small && big && ocache_owner(small) && !ocache_in_window(big) &&
             heap_bytes_used() >= used0 + 8192u;
    for (uint32_t i = 0; ok && i < 8192; i++) big[i] = (uint8_t)(i * 7);
    for (uint32_t i = 0; ok && i < 8192; i++) if (big[i] != (uint8_t)(i * 7)) ok = 0;
    if (small) kfree(small);
    if (big) kfree(big);
    return ok && heap_bytes_used() == used0;
}

/* Thread 0 left the image's stack for a guarded one (kmain): below it is a
   guard page that is not mapped and that kstack knows is thread 0's. */
static int t_main_stack_guarded(void) {
    uintptr_t rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    uintptr_t top = main_stack_top(), lo = top - KSTACK_MAIN_SIZE;
    return thread_current_id() == 0 && rsp < top && rsp >= lo &&
           lo >= KSTACK_REGION_BASE && top <= KSTACK_REGION_BASE + KSTACK_REGION_SIZE &&
           paging_get_phys(lo - PAGE_SIZE) == 0 && kstack_guard_owner(lo - PAGE_SIZE) == 0;
}

/* Interrupts on, and the tick advancing. A spin, not hlt: with the tick
   broken, hlt waits forever and the test would stop the boot, not fail. Ten
   million rounds are seconds; five ticks are 5 ms. */
static int ticks_advance(uint32_t n) {
    if (!arch_irqs_enabled()) return 0;
    uint32_t t0 = timer_ticks();
    for (uint32_t i = 0; i < 10000000u; i++) {
        if (timer_ticks() - t0 >= n) return 1;
        arch_cpu_relax();
    }
    return 0;
}

/* The event source interrupting, n times: the TSC clock advances without it,
   so ticks_advance alone no longer says the interrupt comes. Each wait asks
   for one 1 ms out, as a deadline would (the PIT's come anyway). Bounded by
   the clock and by a count, should the clock be the tick and the tick dead. */
static int events_come(uint32_t n) {
    if (!arch_irqs_enabled()) return 0;
    uint64_t e0 = timer_events(), t0 = timer_now_ns();
    for (uint32_t i = 0; i < 10000000u; i++) {
        if (timer_events() - e0 >= n) return 1;
        if (timer_now_ns() - t0 > 100 * TIMER_NS_PER_TICK) return 0;
        timer_deadline_hint(timer_now_ns() + TIMER_NS_PER_TICK);
        arch_cpu_relax();
    }
    return 0;
}

static int t_ticks(void) { return ticks_advance(5) && events_come(5); }

/* irq_controller_init took over: the 8259 is masked outright, the I/O APIC
   routes the PIT's tick, and its EOI goes to the local APIC - an EOI sent to
   the wrong controller stops the tick after the first one. The PIT is the
   fallback once the local APIC's timer is the source, so it is put back for
   the test. */
static int t_io_apic(void) {
    if (!(lapic_active() && apic_active() && pic_get_masks() == 0xFFFF)) return 0;
    int back = timer_use_pit_for_test(1);
    uint64_t p0 = timer_pit_ticks();
    int ok = timer_source() == TIMER_SOURCE_PIT && events_come(5) &&
             timer_pit_ticks() - p0 >= 5;
    if (back) timer_use_pit_for_test(0);
    return ok;
}

/* The fallback runs the same expiries: a sleep on the PIT, as t_sleep. */
static int t_sleep(void);
static int t_pit_sleep(void) {
    if (!timer_use_pit_for_test(1)) {
        klog("KTEST", SEV_WARN, "PIT fallback untested: it is the source already");
        return t_sleep();
    }
    int ok = timer_source() == TIMER_SOURCE_PIT && t_sleep();
    timer_use_pit_for_test(0);
    return ok;
}

/* Where Hyper-V grants its frequency MSRs (CPUID 0x40000003: EAX bit 11,
   EDX bit 8), the clock runs at the rate it states, not the PIT's emulated
   count, which ran 0.85-1.1% slow there (1.8c): either side of the report's
   1% bound, so that bound cannot be what holds it. The grant is read from
   CPUID here, not through arch_cycles_stated_hz, so a broken one cannot
   excuse itself. Anywhere else there is nothing to hold. */
static int t_stated_tsc_rate(void) {
    uint32_t r[4];
    cpuid_read(1, 0, r);
    int hv = (r[2] & (1u << 31)) != 0;
    if (hv) {
        cpuid_read(0x40000000u, 0, r);
        hv = r[0] >= 0x40000003u && r[1] == 0x7263694du && r[2] == 0x666f736fu
             && r[3] == 0x76482074u;                      /* "Microsoft Hv" */
    }
    if (hv) { cpuid_read(0x40000001u, 0, r); hv = r[0] == 0x31237648u; }   /* "Hv#1" */
    if (hv) { cpuid_read(0x40000003u, 0, r); hv = (r[0] & (1u << 11)) && (r[3] & (1u << 8)); }
    if (!hv) return 1;
    uint64_t stated = rdmsr(0x40000022u);
    if (timer_tsc_by()[0] != 'h' || timer_tsc_hz() != stated) {
        klog_u32("KTEST", SEV_FAIL, "the clock is not Hyper-V's stated rate, MHz: ",
                 (uint32_t)(timer_tsc_hz() / 1000000u), LOG_COLOR_VALUE, "");
        return 0;
    }
    return 1;
}

/* The clock is the TSC once it can be: it moves between interrupts, in steps
   far finer than the tick, and never back. A CPU whose counter could not be
   a clock leaves the tick, and says so. */
static int t_clock_tsc(void) {
    if (!timer_clock_is_tsc()) {
        if (arch_cycles_constant()) return 0;
        klog("KTEST", SEV_WARN, "TSC clock untested: no usable TSC");
        return 1;
    }
    if (timer_tsc_hz() < 1000000ull) return 0;
    uint32_t f = arch_irq_save();   /* no interrupt can move it: the TSC must */
    uint64_t a = timer_now_ns();
    int fine = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t b = timer_now_ns();
        if (b < a) { arch_irq_restore(f); return 0; }
        if (b > a && b - a < TIMER_NS_PER_TICK / 10) fine = 1;
        a = b;
    }
    arch_irq_restore(f);
    return fine;
}

/* The local APIC's timer took over from the PIT, and the PIT is quiet:
   masked, not merely ignored. */
static int t_apic_source(void) {
    if (!lapic_active() || !timer_clock_is_tsc()) {
        klog("KTEST", SEV_WARN, "APIC timer untested: no local APIC or TSC clock");
        return timer_source() == TIMER_SOURCE_PIT;
    }
    if (timer_source() != TIMER_SOURCE_APIC || timer_evtimer_hz() < 100000ull) return 0;
    uint64_t p0 = timer_pit_ticks();
    thread_sleep_ms(20);
    return timer_pit_ticks() == p0;
}

/* Idle without a fixed tick: a 100 ms sleep with nothing else due is a few
   interrupts, not the PIT's hundred. Others asleep may add theirs (the
   bound leaves them 20). */
static int t_idle_quiet(void) {
    if (timer_source() != TIMER_SOURCE_APIC) {
        klog("KTEST", SEV_WARN, "tickless idle untested: the PIT is the source");
        return 1;
    }
    uint64_t e0 = timer_events();
    thread_sleep_ms(100);
    uint64_t n = timer_events() - e0;
    return n >= 1 && n <= 20;
}

/* A deadline is met when it comes, not at the next millisecond. Eight
   deadlines a ms apart plus 125 us more each, alternately a timer object
   firing and a wait timing out at its deadline: on a 1 ms tick their
   lateness spreads over most of a millisecond, at the APIC's deadline it is
   the same wake cost each time. The cost itself is no measure here: the
   verification boot's -icount shift=6 makes every instruction 64 virtual
   ns, so the interrupt-to-thread path alone is some 300 us; the bound on
   it is only below a tick. */
static int t_deadline_precise(void) {
    if (timer_source() != TIMER_SOURCE_APIC) {
        klog("KTEST", SEV_WARN, "deadline precision untested: the PIT is the source");
        return 1;
    }
    const uint64_t us = 1000;
    struct ktimer *t = ktimer_create(0);
    if (!t) return 0;
    uint64_t lo = TIMER_NEVER, hi = 0;
    int ok = 1;
    for (uint64_t i = 0; i < 8 && ok; i++) {
        uint64_t dl = timer_now_ns() + 1500 * us + i * 125 * us;
        int rc, want;
        if (i & 1) {          /* fired and not re-armed: the wait times out */
            ktimer_cancel(t);
            rc = kobj_wait(&t->hdr, SIGNAL_TIMER_FIRED, dl, 0);
            want = E_TIMED_OUT;
        } else {
            ktimer_set(t, dl);
            rc = kobj_wait(&t->hdr, SIGNAL_TIMER_FIRED, KWAIT_NEVER, 0);
            want = E_OK;
        }
        uint64_t at = timer_now_ns();
        ok = rc == want && at >= dl && at - dl < 1000 * us;
        if (ok && at - dl < lo) lo = at - dl;
        if (ok && at - dl > hi) hi = at - dl;
    }
    kobj_unref(&t->hdr);
    if (ok) klog_u32("KTEST", SEV_INFO, "deadline lateness spread, us: ",
                     (uint32_t)((hi - lo) / us), LOG_COLOR_VALUE, "");
    return ok && hi - lo < 250 * us;
}

/* Slack: a timer due at 8 ms that may wait 5 fires with one due at 10, not
   at its own deadline and not at its latest; one interrupt serves both. */
static int t_slack(void) {
    if (timer_source() != TIMER_SOURCE_APIC) {
        klog("KTEST", SEV_WARN, "slack untested: the PIT is the source");
        return 1;
    }
    const uint64_t ms = TIMER_NS_PER_TICK;
    struct ktimer *a = ktimer_create(0), *b = ktimer_create(5 * ms);
    int ok = a && b;
    if (ok) {
        uint64_t now = timer_now_ns();
        ktimer_set(a, now + 10 * ms);
        ktimer_set(b, now + 8 * ms);
        uint32_t seen = 0;
        int rc = kobj_wait(&b->hdr, SIGNAL_TIMER_FIRED, KWAIT_NEVER, 0);
        uint64_t at = timer_now_ns();
        rc = rc == E_OK ? kobj_wait(&a->hdr, SIGNAL_TIMER_FIRED, 0, &seen) : rc;
        ok = rc == E_OK && (seen & SIGNAL_TIMER_FIRED) &&
             at >= now + 10 * ms && at < now + 11 * ms;
    }
    if (a) kobj_unref(&a->hdr);
    if (b) kobj_unref(&b->hdr);
    return ok;
}

/* A tick is a millisecond by a clock the PIT does not drive (ktest.c's
   test_tick_rate): 100 ticks must be 100 ms of the ACPI PM timer, within a
   fifth. Left at the BIOS's 18.2 Hz every tick would be 55 ms. */
static int t_tick_rate(void) {
    uint32_t p0, p1;
    if (acpi_pm_timer_read(&p0) != 0) {
        klog("KTEST", SEV_WARN, "tick rate untested: no ACPI PM timer");
        return 1;
    }
    const uint32_t want = ACPI_PM_TIMER_HZ / 10;
    const uint32_t limit = ACPI_PM_TIMER_HZ * 2;
    uint32_t t0 = timer_ticks();
    while (timer_ticks() == t0) {                        /* align to a tick edge */
        acpi_pm_timer_read(&p1);
        if (((p1 - p0) & 0xFFFFFFu) > limit) return 0;
    }
    t0 = timer_ticks();
    acpi_pm_timer_read(&p0);
    for (;;) {
        acpi_pm_timer_read(&p1);
        uint32_t pm = (p1 - p0) & 0xFFFFFFu;
        if (timer_ticks() - t0 >= 100) return pm >= want - want / 5 && pm <= want + want / 5;
        if (pm > limit) return 0;
    }
}

/* ktest.c's test_clock_sleep: a 50 ms sleep is 50 to 100 ticks, never fewer. */
static int t_sleep(void) {
    uint32_t t0 = timer_ticks();
    thread_sleep_ms(50);
    uint32_t dt = timer_ticks() - t0;
    if (dt < 50 || dt > 100) return 0;
    t0 = timer_ticks();
    thread_sleep_ms(0);                                  /* a yield, not a wait */
    return timer_ticks() - t0 <= 50;
}

static void settle(void) {
    for (int i = 0; i < 8 && sched_active_count() > 1; i++) yield();
}

static volatile int coop_a, coop_b;
static void coop_thread_a(void) { for (int i = 0; i < 3; i++) { coop_a++; yield(); } }
static void coop_thread_b(void) { for (int i = 0; i < 3; i++) { coop_b++; yield(); } }

static int t_coop(void) {
    coop_a = coop_b = 0;
    if (thread_create("coop_a", coop_thread_a) < 0) return 0;
    if (thread_create("coop_b", coop_thread_b) < 0) return 0;
    settle();
    return coop_a == 3 && coop_b == 3 && sched_active_count() == 1;
}

/* context_switch keeps what the ABI says a callee keeps. Thread 0 loads
   rbx and r12-r15 with patterns and yields to a thread that loads them with
   others; back on thread 0 its own must be there. The registers are set and
   read in one asm statement around the call, so the compiler cannot keep
   copies elsewhere. rbp is the frame pointer and is left alone. */
volatile uint64_t ktest64_regs_seen[5];
static volatile int clobber_ran;

static void clobber_thread(void) {
    __asm__ volatile ("movq $0xBAD0, %%rbx\n\t"
                      "movq $0xBAD2, %%r12\n\t"
                      "movq $0xBAD3, %%r13\n\t"
                      "movq $0xBAD4, %%r14\n\t"
                      "movq $0xBAD5, %%r15\n\t"
                      : : : "rbx", "r12", "r13", "r14", "r15");
    clobber_ran = 1;
    yield();
}

static int t_switch_keeps_registers(void) {
    clobber_ran = 0;
    if (thread_create("clobber", clobber_thread) < 0) return 0;
    __asm__ volatile ("movq $0x1111, %%rbx\n\t"
                      "movq $0x2222, %%r12\n\t"
                      "movq $0x3333, %%r13\n\t"
                      "movq $0x4444, %%r14\n\t"
                      "movq $0x5555, %%r15\n\t"
                      "call yield\n\t"
                      "movq %%rbx, ktest64_regs_seen+0(%%rip)\n\t"
                      "movq %%r12, ktest64_regs_seen+8(%%rip)\n\t"
                      "movq %%r13, ktest64_regs_seen+16(%%rip)\n\t"
                      "movq %%r14, ktest64_regs_seen+24(%%rip)\n\t"
                      "movq %%r15, ktest64_regs_seen+32(%%rip)\n\t"
                      : : : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
                            "r10", "r11", "r12", "r13", "r14", "r15", "memory", "cc");
    settle();
    return clobber_ran && ktest64_regs_seen[0] == 0x1111 && ktest64_regs_seen[1] == 0x2222 &&
           ktest64_regs_seen[2] == 0x3333 && ktest64_regs_seen[3] == 0x4444 &&
           ktest64_regs_seen[4] == 0x5555 && sched_active_count() == 1;
}

/* arch_thread_frame's RFLAGS: a new thread starts with interrupts on, even
   when the thread that switched to it had them off. */
static volatile int launched_if;
static void if_thread(void) { launched_if = arch_irqs_enabled() ? 1 : 2; }

static int t_new_thread_irqs_on(void) {
    launched_if = 0;
    if (thread_create("if", if_thread) < 0) return 0;
    uint32_t f = arch_irq_save();
    yield();
    arch_irq_restore(f);
    settle();
    return launched_if == 1 && arch_irqs_enabled();
}

/* ktest.c's test_sched_preempt: two threads that never yield both finish,
   only because the tick switches between them (this thread spins without
   yielding). A million iterations each take about a third of the 5000-tick
   bound under -icount; three million took 5020 ticks once the tick did a
   little more work (1.2a's timers), so the bound was the test. */
static volatile uint32_t pre_a, pre_b;
static void pre_thread_a(void) { for (uint32_t i = 0; i < 1000000u; i++) pre_a = i; }
static void pre_thread_b(void) { for (uint32_t i = 0; i < 1000000u; i++) pre_b = i; }

static int t_preempt(void) {
    pre_a = pre_b = 0;
    if (thread_create("pre_a", pre_thread_a) < 0) return 0;
    if (thread_create("pre_b", pre_thread_b) < 0) return 0;
    /* Bounded by the tick, which keeps counting even if nothing preempts: the
       spinners are then left behind, but the test fails rather than hangs. */
    uint32_t t0 = timer_ticks();
    sched_preempt_enable(5);
    while (sched_active_count() > 1 && timer_ticks() - t0 < 5000) arch_cpu_relax();
    sched_preempt_disable();
    return pre_a != 0 && pre_b != 0 && sched_active_count() == 1;
}

/* ktest.c's test_esp0_follows_thread, for RSP0: a kernel-only thread sees the
   dedicated stack, and thread 0 gets its own back. */
static volatile uintptr_t rsp0_seen;
static void rsp0_thread(void) { rsp0_seen = arch_kernel_stack_current(); }

static int t_rsp0_follows_thread(void) {
    uintptr_t mine = thread_current_kstack_top();
    if (mine == 0 || mine == arch_kernel_stack_default()) return 0;
    rsp0_seen = 0;
    if (thread_create("rsp0", rsp0_thread) < 0) return 0;
    settle();
    return rsp0_seen == arch_kernel_stack_default() &&
           arch_kernel_stack_current() == mine && sched_active_count() == 1;
}

/* FS_BASE, the same way (roadmap 1.5c2): two threads set bases of their own
   and take turns with this one, and after every switch back each finds the
   MSR holding its own, this one its own (none). The two bases are user
   addresses at both ends of the half: nothing here reads through them. */
static volatile int fsb_bad, fsb_done;
static void fsb_run(uint64_t mine) {
    thread_set_tls(thread_current_id(), mine);
    for (int i = 0; i < 4; i++) {
        yield();
        if (arch_tls_base_current() != mine) fsb_bad++;
    }
    fsb_done++;
}
static void fsb_low(void)  { fsb_run(0x0000000000001000ull); }
static void fsb_high(void) { fsb_run(USER_VLIMIT - 0x1000u); }

static int t_tls_follows_thread(void) {
    int me = thread_current_id();
    uint64_t mine = thread_tls(me);
    fsb_bad = fsb_done = 0;
    if (thread_create("fsb_low", fsb_low) < 0) return 0;
    if (thread_create("fsb_high", fsb_high) < 0) return 0;
    for (int i = 0; i < 32 && fsb_done < 2; i++) {
        yield();
        if (arch_tls_base_current() != mine) fsb_bad++;
    }
    settle();
    return fsb_done == 2 && fsb_bad == 0 && arch_tls_base_current() == mine &&
           sched_active_count() == 1;
}

/* The MSI pool: a vector from it reaches the handler it was given, a freed
   one reaches nothing, and the slot is handed out again. The vector is raised
   with int, which is how a device's write arrives at the IDT. */
static volatile int msi_hits;
static volatile uint32_t msi_vector_seen;
static void msi_handler(struct registers *r) { msi_hits++; msi_vector_seen = irq_frame_vector(r); }

static int t_msi_pool(void) {
    msi_hits = 0;
    int v = irq_alloc_msi_vector(msi_handler);
    __asm__ volatile ("int %0" : : "i"(MSI_VECTOR_BASE) : "memory");
    int ok = v == MSI_VECTOR_BASE && msi_hits == 1 && msi_vector_seen == MSI_VECTOR_BASE;
    if (v >= 0) irq_free_msi_vector(v);
    __asm__ volatile ("int %0" : : "i"(MSI_VECTOR_BASE) : "memory");
    ok = ok && msi_hits == 1;
    int w = irq_alloc_msi_vector(msi_handler);
    if (w >= 0) irq_free_msi_vector(w);
    return ok && w == v;
}

/* ---- ring 3 (step 5e) ---- */

/* Each test process's pages, in a space of its own. Below 4 GB, since the
   ABI's user addresses stay 32 bits until step 6. */
#define U_CODE     0x00400000u
#define U_GUARD    0x00401000u      /* never mapped: the stack's guard */
#define U_STACK    0x00402000u
#define U_SHARED   0x00403000u
#define U_SP       (U_STACK + PAGE_SIZE - 16)   /* the argument sits at [rsp] */

#define BLOB(n) extern uint8_t user64_##n##_start[], user64_##n##_end[]
BLOB(cpl); BLOB(regs); BLOB(write); BLOB(ud2); BLOB(read);
BLOB(exec_stack); BLOB(write_code); BLOB(overflow); BLOB(spin);
extern uint8_t user64_write_msg[], user64_write_msg_end[];
extern void syscall64_entry(void);

#define CPL_EXIT   (USER_CODE_SEL | USER_DATA_SEL << 8 | 1u << 16)

/* The fault hook, in place of usermode.c's while a test process runs
   (run_user): it notes what arrived, which usermode.c's only logs, and leaves
   the same way, back out of enter_usermode with USERMODE_FAULTED. */
static volatile struct {
    uint32_t count;
    uint64_t vector, error, rip, cr2;
} ufault;

static void user_fault(struct registers *r) {
    ufault.count++;
    ufault.vector = r->vector;
    ufault.error  = r->error;
    ufault.rip    = r->rip;
    ufault.cr2    = read_cr2();
    return_to_kernel(USERMODE_FAULTED, thread_current_usave());
}

struct urun {
    const uint8_t *code;
    uint32_t len;
    uint64_t arg;               /* at [rsp] on entry */
    paddr_t  shared;            /* mapped at U_SHARED when not 0 */
    volatile int set_up, done, rc, pid;
    volatile uint64_t code_pte, stack_pte;
};

/* A process's one thread: its code written and then made executable and
   read-only (W^X), its stack writable and no-execute with nothing mapped
   below, and into ring 3. The space, and every frame mapped in it, goes when
   the thread does. */
static void urun_thread(void) {
    struct urun *u = thread_current_arg();
    u->pid = thread_current_pid();
    paddr_t c = pmm_alloc();
    if (!c || paging_map_user(U_CODE, c, VM_WRITE) != 0) return;
    paddr_t s = pmm_alloc();
    if (!s || paging_map_user(U_STACK, s, VM_WRITE) != 0) return;
    for (uint32_t i = 0; i < u->len; i++) ((volatile uint8_t *)(uintptr_t)U_CODE)[i] = u->code[i];
    if (paging_map_user(U_CODE, c, VM_EXEC) != 0) return;
    if (u->shared && paging_map_user(U_SHARED, u->shared, VM_WRITE) != 0) return;
    if (syspage_map(SYSPAGE_VIRT, NULL) != 0) return;   /* the calls go through it */
    *(volatile uint64_t *)(uintptr_t)U_SP = u->arg;
    u->code_pte  = entry_of(U_CODE);
    u->stack_pte = entry_of(U_STACK);
    u->set_up = 1;
    u->rc = enter_usermode(U_CODE, U_SP, thread_current_usave(), SYSPAGE_VIRT);
    if (u->shared) paging_unmap(U_SHARED);   /* the test's frame, not the space's */
    u->done = 1;
}

static void urun_init(struct urun *u, const uint8_t *start, const uint8_t *end, uint64_t arg) {
    u->code = start;
    u->len = (uint32_t)(end - start);
    u->arg = arg;
    u->shared = 0;
    u->set_up = u->done = u->pid = 0;
    u->rc = 0x7FFFFFFF;
    u->code_pte = u->stack_pte = 0;
}

static int urun_start(struct urun *u) {
    paddr_t space;
    if (addr_space_create(&space) != 0) return -1;
    int id = thread_create_process("ring3", urun_thread, 0);
    if (id < 0) { addr_space_release(space); return -1; }
    thread_mark_user(id);
    thread_set_space(id, space);   /* the process's from here */
    thread_set_arg(id, u, 0);
    thread_set_caps(id, GRANT_CONSOLE);
    thread_start(id);
    return id;
}

/* Until the thread has finished and been reaped, process and all. */
static int urun_wait(const struct urun *u, uint32_t threads, uint32_t procs) {
    for (int i = 0; i < 2000; i++) {
        if (u->done && sched_thread_count() == threads && proc_live() == procs) return 1;
        thread_sleep_ms(1);
    }
    return 0;
}

/* Run [start, end) in ring 3 in a process of its own: 1 if it was set up, ran
   to an exit or a fault, and is gone. */
static int run_user(struct urun *u, const uint8_t *start, const uint8_t *end, uint64_t arg) {
    urun_init(u, start, end, arg);
    uint32_t n0 = sched_thread_count(), p0 = proc_live();
    set_user_fault_hook(user_fault);
    int ok = urun_start(u) >= 0 && urun_wait(u, n0, p0) && u->set_up;
    usermode_register_fault_handler();   /* usermode.c's again, as kmain left it */
    return ok;
}

static int t_syscall_msrs(void) {
    return (rdmsr(MSR_EFER) & EFER_SCE) && rdmsr(MSR_STAR) == STAR_VALUE &&
           rdmsr(MSR_LSTAR) == (uint64_t)(uintptr_t)syscall64_entry &&
           rdmsr(MSR_FMASK) == FMASK_VALUE &&
           /* what sysretq loads: STAR[63:48] + 8 and + 16, as AMD loads them,
              with no RPL forced (Intel's OR of 3 is then a no-op); QEMU is
              Intel's, so only this arithmetic shows AMD's SS */
           ((STAR_VALUE >> 48) + 8) == USER_DATA_SEL &&
           ((STAR_VALUE >> 48) + 16) == USER_CODE_SEL;
}

static int t_ring3_cpl(void) {
    struct urun u;
    return run_user(&u, user64_cpl_start, user64_cpl_end, 0) && u.rc == (int)CPL_EXIT;
}

static int t_ring3_mappings(void) {
    struct urun u;
    if (!run_user(&u, user64_cpl_start, user64_cpl_end, 0)) return 0;
    uint64_t c = u.code_pte, s = u.stack_pte;
    return (c & PTE64_USER) && !(c & PTE64_WRITE) && !(c & PTE64_NX) &&
           (s & PTE64_USER) && (s & PTE64_WRITE) && (s & PTE64_NX);
}

static int t_ring3_regs(void) {
    struct urun u;
    return run_user(&u, user64_regs_start, user64_regs_end, 0) &&
           ((uint32_t)u.rc >> 16) == 0 && (u.rc & 0xFFFF) == (u.pid & 0xFFFF);
}

/* Two writes and an exit, each counted at the gate, at the dispatcher and at
   the recording point; the exit never returns, so it has no exit record. */
static int t_ring3_write(void) {
    struct sysrec_stats s0, s1;
    sysrec_snapshot(&s0);
    uint32_t t0 = syscall_trapped_count(), d0 = syscall_dispatch_count();
    struct urun u;
    if (!run_user(&u, user64_write_start, user64_write_end, 0)) return 0;
    sysrec_snapshot(&s1);
    uint32_t len = (uint32_t)(user64_write_msg_end - user64_write_msg);
    /* The exit came in with all five registers, whole: the routine loads
       64-bit markers into rsi, rdx, r10 and r8 (usermode.asm), which SYS_EXIT
       does not read, so a gate that dropped a high half (a u64 argument) fails. */
    int carried = s1.in_num == SYS_EXIT && s1.in_args[0] == (uint32_t)u.rc;
    for (uint32_t i = 1; i < SYSCALL_ARGS; i++)
        carried = carried && s1.in_args[i] == ((uint64_t)(0xA1u + i) << 56 | (0xC0DE0001u + i));
    return (uint32_t)(u.rc & 0xFFFF) == len && ((u.rc >> 16) & 0xFF) == 0xFF &&
           syscall_trapped_count() - t0 == 3 && syscall_dispatch_count() - d0 == 3 &&
           s1.entered - s0.entered == 3 && s1.exited - s0.exited == 2 &&
           s1.last_num == SYS_CONSOLE_WRITE && carried;
}

/* The process faulted as described, and enter_usermode came back. */
static int user_faults(const uint8_t *start, const uint8_t *end, uint64_t arg,
                       uint64_t vector, uint64_t error, uint64_t cr2) {
    struct urun u;
    uint32_t n = ufault.count;
    return run_user(&u, start, end, arg) && u.rc == USERMODE_FAULTED &&
           ufault.count == n + 1 && ufault.vector == vector && ufault.error == error &&
           (vector != VEC_PAGE_FAULT || ufault.cr2 == cr2);
}

static int t_ring3_fault(void) {
    return user_faults(user64_ud2_start, user64_ud2_end, 0, VEC_INVALID_OPCODE, 0, 0) &&
           ufault.rip == U_CODE;
}

static int t_ring3_kernel_read(void) {
    uint64_t at = (uint64_t)(uintptr_t)&expect;
    return user_faults(user64_read_start, user64_read_end, at,
                       VEC_PAGE_FAULT, PF_PRESENT | PF_USER, at);
}

static int t_ring3_stack_nx(void) {
    return user_faults(user64_exec_stack_start, user64_exec_stack_end, 0,
                       VEC_PAGE_FAULT, PF_PRESENT | PF_USER | PF_FETCH, U_SP);
}

static int t_ring3_code_readonly(void) {
    return user_faults(user64_write_code_start, user64_write_code_end, 0,
                       VEC_PAGE_FAULT, PF_PRESENT | PF_WRITE | PF_USER, U_CODE);
}

static int t_ring3_stack_guard(void) {
    return user_faults(user64_overflow_start, user64_overflow_end, 0,
                       VEC_PAGE_FAULT, PF_WRITE | PF_USER, U_STACK - 8);
}

/* A kernel stack that overflows runs into its guard page, and the CPU cannot
   push that page fault's frame there either: a double fault, on IST 1. With
   interrupts off, since a tick taken at that RSP would be lost with its EOI. */
static int t_kernel_stack_overflow(void) {
    uintptr_t lo, hi;
    gdt64_ist_stack(IST_DOUBLE_FAULT, &lo, &hi);
    uintptr_t bottom = main_stack_top() - KSTACK_MAIN_SIZE;
    uint32_t flags = arch_irq_save();
    arm(VEC_DOUBLE_FAULT);
    __asm__ volatile ("leaq 1f(%%rip), %%rax\n\t"
                      "movq %%rax, %0\n\t"
                      "movq %%rsp, %1\n\t"
                      "movq %2, %%rsp\n\t"
                      "pushq $0\n"
                      "1:"
                      : "=m"(expect.resume_rip), "=m"(expect.resume_rsp)
                      : "r"(bottom) : "rax", "memory");
    disarm();
    uint64_t cr2 = read_cr2();
    arch_irq_restore(flags);
    return expect.hits == 1 && expect.error == 0 &&
           expect.frame >= lo && expect.frame < hi && cr2 == bottom - 8;
}

/* Each layer of the user copy on its own: the check refuses an unmapped or a
   read-only buffer before touching it, so takes no fault; the primitive,
   unchecked, takes the fault and comes back with 1; a good copy takes none. */
static int t_usercopy(void) {
    paddr_t root;
    if (addr_space_create(&root) != 0) return 0;
    addr_space_switch(root);
    paddr_t ro = pmm_alloc();
    uint8_t buf[16] = { 0 };
    uint32_t r0 = usercopy_faults_recovered();
    int ok = ro && paging_map_user(U_CODE, ro, 0) == 0;
    ok = ok && user_copy_in(buf, U_STACK, sizeof buf) == E_BAD_ADDRESS &&
               user_copy_out(U_CODE, buf, sizeof buf) == E_BAD_ADDRESS &&
               usercopy_faults_recovered() == r0;
    ok = ok && user_copy_bytes(ktest_umem(U_CODE), buf, sizeof buf) == 1 &&
               user_copy_bytes(buf, ktest_umem(U_STACK), sizeof buf) == 1 &&
               usercopy_faults_recovered() == r0 + 2;
    ok = ok && user_copy_in(buf, U_CODE, sizeof buf) == 0 &&
               usercopy_faults_recovered() == r0 + 2;
    addr_space_switch(addr_space_kernel());
    addr_space_release(root);   /* frees ro with it */
    return ok;
}

/* Two processes that spin in ring 3 and never make a call until the kernel
   writes a word they share. This thread neither sleeps nor yields while they
   spin, so it runs at all only because the tick took the CPU from ring 3; if
   it never does, the boot stops here rather than failing. */
static int t_ring3_preempted(void) {
    paddr_t sh = pmm_alloc();
    if (!sh) return 0;
    volatile uint32_t *word = direct(sh);
    word[0] = 0;                /* the word they wait for */
    word[1] = 0;                /* how many have started spinning */
    struct urun a, b;
    uint32_t n0 = sched_thread_count(), p0 = proc_live();
    urun_init(&a, user64_spin_start, user64_spin_end, U_SHARED);
    urun_init(&b, user64_spin_start, user64_spin_end, U_SHARED);
    a.shared = b.shared = sh;
    int ok = urun_start(&a) >= 0 && urun_start(&b) >= 0;
    sched_preempt_enable(5);
    while (ok && word[1] < 2) arch_cpu_relax();
    word[0] = 0x5E;
    while (ok && !(a.done && b.done)) arch_cpu_relax();
    sched_preempt_disable();
    ok = ok && urun_wait(&a, n0, p0) && a.rc == 0x5E && b.rc == 0x5E;
    pmm_free(sh);
    return ok;
}

/* After an exit and after a fault, every page, heap block, thread and process
   is back. The earlier tests have run the same path, so the heap and the
   caches have what they keep. */
static int t_ring3_releases(void) {
    uint32_t free0 = pmm_free_count(), heap0 = heap_allocs_live();
    struct urun u, v;
    int ok = run_user(&u, user64_cpl_start, user64_cpl_end, 0) && u.rc == (int)CPL_EXIT &&
             run_user(&v, user64_ud2_start, user64_ud2_end, 0) && v.rc == USERMODE_FAULTED;
    return ok && pmm_free_count() == free0 && heap_allocs_live() == heap0;
}

/* Both sides of the bound: the user half's last page is refused, the one
   below it is not (paging.c says why). */
static int t_user_top_page(void) {
    paddr_t root;
    if (addr_space_create(&root) != 0) return 0;
    addr_space_switch(root);
    paddr_t f = pmm_alloc();
    int ok = f && paging_map_user(USER_VLIMIT - PAGE_SIZE, f, 0) == -1 &&
                  paging_map_user(USER_VLIMIT - 2 * PAGE_SIZE, f, 0) == 0;
    addr_space_switch(addr_space_kernel());
    addr_space_release(root);   /* frees f with it */
    return ok;
}

/* ---- the CPU's own data (cpu/percpu.h, 1.9b) ---- */

/* `int vec` with ring 3's GS base swapped in, as an NMI, #DB or #MC that
   lands in the syscall entry before its swapgs would find it: returns GS_BASE
   once the handler is back, which must be ring 3's again. Interrupts are off
   throughout: an ordinary one here trusts CS and would run on ring 3's base. */
static uint64_t int_with_user_gs(uint64_t vec) {
    uint32_t lo, hi;
    uint32_t f = arch_irq_save();
    arm(vec);
    switch (vec) {
#define INT_USER_GS(n) \
    case n: __asm__ volatile ("swapgs\n\tint $" #n "\n\trdmsr\n\tswapgs" \
                              : "=a"(lo), "=d"(hi) : "c"(MSR_GS_BASE) : "memory"); break;
    INT_USER_GS(1)
    INT_USER_GS(2)
    INT_USER_GS(18)
#undef INT_USER_GS
    default: lo = hi = 0xFFFFFFFFu;
    }
    disarm();
    arch_irq_restore(f);
    return (uint64_t)hi << 32 | lo;
}

static volatile int      cl_ran;
static volatile int32_t  cl_seen;
static void cl_thread(void) { cl_seen = cpu_thread(); cl_ran = thread_current_id(); }

/* Returns the step that failed (logged), or 0. */
static int cpu_local_steps(void) {
    struct cpu *me = cpu_this();
    if ((uint64_t)(uintptr_t)me != rdmsr(MSR_GS_BASE) || me != &cpu_boot) return 1;
    if (me->self != me) return 2;
    if (rdmsr(MSR_KERNEL_GS_BASE) >> 63) return 3;   /* ring 3's: the low half */
    if (cpu_thread() != thread_current_id()) return 4;
    if (me->kstack != arch_kernel_stack_current()) return 5;

    /* the running thread follows a switch */
    cl_ran = 0; cl_seen = -1;
    int id = thread_create("cpu-local", cl_thread);
    if (id < 0) return 6;
    settle();
    if (cl_ran != id || cl_seen != id) return 7;
    if (cpu_thread() != thread_current_id()) return 8;
    if (me->kstack != arch_kernel_stack_current()) return 9;

    /* an IST vector from the kernel: no swap, the kernel's base throughout */
    arm(VEC_DEBUG);
    __asm__ volatile ("int $1" ::: "memory");
    disarm();
    if (expect.hits != 1 || expect.gs != (uint64_t)(uintptr_t)me) return 10;
    if (rdmsr(MSR_GS_BASE) != (uint64_t)(uintptr_t)me) return 11;

    /* and with ring 3's base in: swapped in for the handler, back after */
    static const uint64_t vecs[] = { VEC_DEBUG, VEC_NMI, VEC_MACHINE_CHECK };
    for (int i = 0; i < 3; i++) {
        uint64_t after = int_with_user_gs(vecs[i]);
        if (expect.hits != 1 || expect.gs != (uint64_t)(uintptr_t)me) return 12 + 2 * i;
        if (after >> 63) return 13 + 2 * i;   /* 12..17 */
    }
    if (rdmsr(MSR_GS_BASE) != (uint64_t)(uintptr_t)me) return 18;
    return 0;
}

static int t_cpu_local(void) {
    int step = cpu_local_steps();
    if (step) klog_u32("KTEST", SEV_FAIL, "cpu local: step ", (uint32_t)step, LOG_COLOR_VALUE, "");
    return step == 0;
}


/* ---- 1.9d: the APs, started by the MADT's IDs and parked (smp.c) ----
 *
 * The verification boot gives QEMU four CPUs at APIC IDs 0, 1, 2 and 4 and
 * lists two more disabled (BootVerifier.X86_64Smp), so a start by count,
 * by index or of a disabled entry fails these. On one CPU they pass with
 * nothing to compare. Every check of an AP's own state is read on the AP
 * (smp_call), where cpu_this() and the registers are its. */

static int smp_fail(const char *what, unsigned cpu, int step) {
    klog_u32("KTEST", SEV_FAIL, what, (uint32_t)step, LOG_COLOR_VALUE, "");
    klog_child_u32("on CPU", cpu, LOG_COLOR_VALUE, "");
    return 0;
}

static int listed(uint32_t id) {
    const struct madt_cpu *list;
    uint32_t n = apic_cpus(&list, 0);
    for (uint32_t i = 0; i < n; i++) if (list[i].apic_id == id) return 1;
    return 0;
}

static int t_smp_started(void) {
    const struct madt_cpu *list;
    uint32_t n = apic_cpus(&list, 0);
    if (cpu_count() != n) return smp_fail("smp started: step ", cpu_count(), 1);
    if (cpu_at(0) != &cpu_boot || cpu_boot.hw_id != lapic_id() || !listed(cpu_boot.hw_id))
        return smp_fail("smp started: step ", 0, 2);
    for (unsigned i = 1; i < cpu_count(); i++) {
        struct cpu *c = cpu_at(i);
        if (c->self != c || c->index != i || c->state != CPU_RUNNING)
            return smp_fail("smp started: step ", i, 3);
        if (!listed(c->hw_id)) return smp_fail("smp started: step ", i, 4);
        for (unsigned j = 0; j < i; j++)
            if (cpu_at(j)->hw_id == c->hw_id) return smp_fail("smp started: step ", i, 5);
    }
    /* and every listed ID is a running CPU's: none was started by another */
    for (uint32_t k = 0; k < n; k++) {
        int found = 0;
        for (unsigned i = 0; i < cpu_count(); i++) if (cpu_at(i)->hw_id == list[k].apic_id) found = 1;
        if (!found) return smp_fail("smp started: step ", k, 6);
    }
    return 1;
}

/* The trampoline's GDT (smp_tramp.asm): every descriptor is marked
   accessed, or loading one with paging on writes to a page mapped read-only
   and the AP triple-faults. TCG never faulted on that write; Hyper-V does
   (1.9f). Read from the template, which every AP's copy is. */
extern const uint8_t smp_tramp_gdt[];
static int t_tramp_gdt_accessed(void) {
    for (unsigned i = 1; i < 4; i++) {
        uint64_t d = 0;
        for (unsigned b = 0; b < 8; b++) d |= (uint64_t)smp_tramp_gdt[i * 8 + b] << (8 * b);
        if (!(d & (1ull << 47))) return smp_fail("tramp gdt: step ", i, 1);   /* present */
        if (!(d & (1ull << 40))) return smp_fail("tramp gdt: step ", i, 2);   /* accessed */
    }
    return 1;
}

/* What an AP reads of itself. */
struct ap_seen {
    uint32_t  apic_id, svr, tpr, lvt;
    uint64_t  gs, kgs, tss_desc, pat, efer, star, lstar, fmask, cr0, cr3, cr4;
    struct dt_ptr gdt, idt;
    uint16_t  tr;
    uintptr_t tss, rsp0, kstack, ist_lo[IST_STACKS], ist_hi[IST_STACKS];
    uint32_t  index;
};

static void ap_read(void *arg) {
    struct ap_seen *s = arg;
    s->apic_id = lapic_id();
    s->svr = lapic_svr(); s->tpr = lapic_tpr(); s->lvt = lapic_lvt_timer();
    s->gs = rdmsr(MSR_GS_BASE); s->kgs = rdmsr(MSR_KERNEL_GS_BASE);
    __asm__ volatile ("sgdt %0" : "=m"(s->gdt));
    __asm__ volatile ("sidt %0" : "=m"(s->idt));
    __asm__ volatile ("str %0" : "=r"(s->tr));
    s->tss_desc = gdt64_tss_descriptor();
    s->tss = gdt64_tss_base();
    for (unsigned k = 0; k < IST_STACKS; k++) gdt64_ist_stack(k + 1, &s->ist_lo[k], &s->ist_hi[k]);
    s->pat = cpu_has(CPU_PAT) ? rdmsr(MSR_PAT) : 0;
    s->efer = rdmsr(MSR_EFER); s->star = rdmsr(MSR_STAR);
    s->lstar = rdmsr(MSR_LSTAR); s->fmask = rdmsr(MSR_FMASK);
    __asm__ volatile ("mov %%cr0, %0" : "=r"(s->cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(s->cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(s->cr4));
    s->rsp0 = gdt64_rsp0();
    s->kstack = cpu_this()->kstack;
    s->index = cpu_this()->index;
}

static int ap_tables_steps(unsigned i) {
    struct cpu *c = cpu_at(i);
    const struct smp_ap_stacks *st = smp_stacks_of(i);
    struct ap_seen s = {0};
    if (smp_call(i, ap_read, &s, 100000000ull)) return 1;
    if (s.apic_id != c->hw_id) return 2;                     /* the CPU started by that ID */
    if (s.gs != (uint64_t)(uintptr_t)c || s.index != i || (s.kgs >> 63)) return 3;
    if (s.gdt.base == gdt64_base() || s.gdt.limit != 7 * 8 - 1) return 4;
    if (s.tr != GDT64_TSS || ((s.tss_desc >> 40) & 0xFF) != 0x8B || s.tss == gdt64_tss_base()) return 5;
    for (unsigned k = 0; k < IST_STACKS; k++) {
        uintptr_t blo, bhi;
        gdt64_ist_stack(k + 1, &blo, &bhi);
        if (s.ist_lo[k] != st->ist_base[k] || s.ist_hi[k] != st->ist_base[k] + GDT64_IST_STACK_SIZE ||
            s.ist_lo[k] == blo) return 6;
    }
    if (s.idt.base != idt64_base() || s.idt.limit != 256 * 16 - 1) return 7;
    if (cpu_has(CPU_PAT) && s.pat != 0x0007040100070406ull) return 8;   /* PAT_CXK */
    if (!(s.efer & EFER_SCE) || s.star != STAR_VALUE ||
        s.lstar != (uint64_t)(uintptr_t)syscall64_entry || s.fmask != FMASK_VALUE) return 9;
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    if ((s.cr3 & PTE64_ADDR) != addr_space_kernel() || s.cr0 != cr0 || s.cr4 != cr4) return 10;
    if (!(s.svr & (1u << 8)) || (s.svr & 0xFF) != 0xFF || s.tpr != 0 ||
        (s.lvt & 0xFF) != LAPIC_TIMER_VECTOR || !(s.lvt & (1u << 16))) return 11;
    if (s.rsp0 != st->rsp0_top || s.kstack != st->rsp0_top) return 12;
    return 0;
}

static int t_smp_ap_tables(void) {
    for (unsigned i = 1; i < cpu_count(); i++) {
        int step = ap_tables_steps(i);
        if (step) return smp_fail("smp ap tables: step ", i, step);
    }
    return 1;
}

/* An AP's own timer: two one-shot counts, each interrupting that CPU once
   (the second only if the first was acknowledged at its local APIC). */
struct ap_timer { uint32_t before, after, lvt; };

static void ap_timer(void *arg) {
    struct ap_timer *t = arg;
    unsigned me = cpu_this()->index;
    t->before = smp_timer_hits(me);
    for (int round = 0; round < 2; round++) {
        uint32_t want = t->before + (uint32_t)round + 1;
        lapic_timer_oneshot(1000, 1);           /* 16 us at QEMU's 1 GHz */
        uint64_t end = arch_cycles64() + 400000000ull;
        arch_irq_enable();
        while (smp_timer_hits(me) < want && arch_cycles64() < end) arch_cpu_relax();
        arch_irq_disable();
    }
    lapic_timer_oneshot(0, 0);
    t->lvt = lapic_lvt_timer();
    t->after = smp_timer_hits(me);
}

static int t_smp_ap_timer(void) {
    for (unsigned i = 1; i < cpu_count(); i++) {
        struct ap_timer t = {0};
        if (smp_call(i, ap_timer, &t, 2000000000ull)) return smp_fail("smp ap timer: step ", i, 1);
        if (t.after != t.before + 2) return smp_fail("smp ap timer: step ", i, 2);
        if (!(t.lvt & (1u << 16)))   /* masked again */ return smp_fail("smp ap timer: step ", i, 3);
    }
    if (smp_timer_hits(0) != 0) return smp_fail("smp ap timer: step ", 0, 4);   /* the boot CPU's never counted here */
    return 1;
}

/* The TSC agrees on every CPU (1.9e3): each AP as bring-up left it and at
   a meeting now (steps 1, 2); then the last AP's is put out, ahead (3-5)
   and behind (6-8), and the meeting finds it out and sets it right; after
   which the clock it reads falls between two of the boot CPU's (9); its
   clock moves with its offset (10); and the boot CPU and a CPU past the
   last have no bring-up record (11). */
#define TSC_TEST_MARGIN 4096u      /* arch/smp.h's */
#define TSC_TEST_SLACK_NS 10000ull

static int tsc_within(const struct smp_tsc *t) {
    uint64_t a = (uint64_t)(t->after < 0 ? -t->after : t->after);
    return a <= t->err + TSC_TEST_MARGIN;
}

static void ap_clock(void *arg) { *(uint64_t *)arg = timer_now_ns(); }

#define TSC_TEST_SHIFT_NS 1000000000ull
static void ap_clock_shifted(void *arg) {
    int64_t cycles = (int64_t)timer_tsc_hz();   /* TSC_TEST_SHIFT_NS's */
    cpu_this()->tsc_adjust -= cycles;
    *(uint64_t *)arg = timer_now_ns();
    cpu_this()->tsc_adjust += cycles;
}

static int t_smp_tsc(void) {
    for (unsigned i = 1; i < cpu_count(); i++) {
        const struct smp_tsc *s = smp_tsc_at_start(i);
        if (!s || !tsc_within(s)) return smp_fail("smp tsc: step ", i, 1);
        struct smp_tsc t;
        if (smp_tsc_agree(i, 0, &t) != 0 || !tsc_within(&t)) return smp_fail("smp tsc: step ", i, 2);
    }
    if (cpu_count() < 2) return 1;
    unsigned ap = cpu_count() - 1;
    int64_t skew[2] = { 10000000000ll, -(int64_t)(arch_cycles64() / 2) };
    for (int k = 0; k < 2; k++) {
        struct smp_tsc t;
        if (smp_tsc_agree(ap, skew[k], &t) != 0) return smp_fail("smp tsc: step ", ap, 3 + 3 * k);
        int ahead = t.before > 0, want = skew[k] > 0;
        uint64_t mag = (uint64_t)(t.before < 0 ? -t.before : t.before);
        uint64_t half = (uint64_t)(skew[k] < 0 ? -skew[k] : skew[k]) / 2;
        if (!t.adjusted || ahead != want || mag < half) return smp_fail("smp tsc: step ", ap, 4 + 3 * k);
        if (!tsc_within(&t)) return smp_fail("smp tsc: step ", ap, 5 + 3 * k);
    }
    uint64_t on_ap = 0, b0 = timer_now_ns();
    if (smp_call(ap, ap_clock, &on_ap, 2000000000ull)) return smp_fail("smp tsc: step ", ap, 9);
    uint64_t b1 = timer_now_ns();
    if (on_ap + TSC_TEST_SLACK_NS < b0 || on_ap > b1 + TSC_TEST_SLACK_NS) return smp_fail("smp tsc: step ", ap, 9);
    /* The clock is what takes the offset off: with the AP's put a second's
       cycles back, its clock reads a second or so ahead. */
    b0 = timer_now_ns();
    if (smp_call(ap, ap_clock_shifted, &on_ap, 2000000000ull)) return smp_fail("smp tsc: step ", ap, 10);
    b1 = timer_now_ns();
    if (on_ap < b1 + TSC_TEST_SHIFT_NS / 2 || on_ap > b0 + TSC_TEST_SHIFT_NS * 2)
        return smp_fail("smp tsc: step ", ap, 10);
    if (smp_tsc_at_start(0) || smp_tsc_at_start(cpu_count())) return smp_fail("smp tsc: step ", 0, 11);
    return 1;
}

/* An idle AP is halted, in its idle thread (1.9e2): 50 ms with nothing
   asked wakes no AP, and one call wakes it once, by its one IPI. Calls the
   boot CPU, a CPU past the last and a missing function are refused: -1, as
   a call never answered is, and nothing left in a mailbox, which tells the
   two apart. */
static void ap_nothing(void *arg) { (void)arg; }

static int t_smp_parked(void) {
    if (smp_call(0, ap_nothing, 0, 1000000ull) != -1 || cpu_boot.call ||
        smp_call(cpu_count(), ap_nothing, 0, 1000000ull) != -1 ||
        (cpu_count() > 1 && (smp_call(1, 0, &t_smp_parked, 1000000ull) != -1 ||
                               cpu_at(1)->call_arg == (void *)&t_smp_parked)))
        return smp_fail("smp parked: step ", 0, 1);
    uint32_t wakes[CPU_MAX], irqs[CPU_MAX];
    for (unsigned i = 1; i < cpu_count(); i++) { wakes[i] = cpu_at(i)->wakes; irqs[i] = cpu_at(i)->irqs; }
    thread_sleep_ms(50);
    for (unsigned i = 1; i < cpu_count(); i++)
        if (cpu_at(i)->wakes != wakes[i] || cpu_at(i)->irqs != irqs[i]) return smp_fail("smp parked: step ", i, 2);
    for (unsigned i = 1; i < cpu_count(); i++) {
        if (smp_call(i, ap_nothing, 0, 100000000ull)) return smp_fail("smp parked: step ", i, 3);
        if (cpu_at(i)->wakes != wakes[i] + 1 || cpu_at(i)->irqs != irqs[i] + 1 ||
            cpu_at(i)->state != CPU_RUNNING) return smp_fail("smp parked: step ", i, 4);
    }
    return 1;
}

/* ---- TLB shootdown (1.9e1, arch/smp.h) ----
 * Each test changes a mapping an AP has just used and reads it again on
 * the AP. Two frames, told apart by their first word, are what a stale and
 * a fresh entry show. On fewer CPUs than a test needs it passes with
 * nothing to compare, as the tests above do. */

#define TLB_OLD 0x01D0000001D00000ull
#define TLB_NEW 0x0E50000000E50000ull

static int tlb_frames(paddr_t *a, paddr_t *b) {
    *a = pmm_alloc();
    *b = pmm_alloc();
    if (!*a || !*b) {
        if (*a) pmm_free(*a);
        if (*b) pmm_free(*b);
        return -1;
    }
    *(volatile uint64_t *)direct(*a) = TLB_OLD;
    *(volatile uint64_t *)direct(*b) = TLB_NEW;
    return 0;
}

/* The 4 KiB entry for `virt` under `root`, to be written by hand: the
   control that shows a stale entry is there to be seen. */
static volatile uint64_t *entry_in(paddr_t root, uint64_t virt) {
    uint64_t *t = direct(root);
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        uint64_t e = t[(virt >> shift) & 511u];
        if (!(e & PTE64_PRESENT) || (e & PTE64_LARGE)) return 0;
        t = direct(e & PTE64_ADDR);
    }
    return &t[PT_INDEX(virt)];
}

/* An AP reading a page over and over with interrupts on, so a shootdown's
   IPI reaches it, until told to stop; then once more, after it saw the
   stop. In `space` (0: the kernel's). */
struct tlb_watch {
    uintptr_t va;
    paddr_t   space;
    volatile uint64_t first, last;
    volatile uint32_t reads, stop;
};

static void ap_watch(void *arg) {
    struct tlb_watch *w = arg;
    if (w->space) addr_space_switch(w->space);
    w->first = *(volatile uint64_t *)w->va;
    uint64_t end = arch_cycles64() + 4000000000ull;   /* a bound if never told */
    arch_irq_enable();
    while (!w->stop && arch_cycles64() < end) {
        w->last = *(volatile uint64_t *)w->va;
        __atomic_fetch_add(&w->reads, 1u, __ATOMIC_RELEASE);
        arch_cpu_relax();
    }
    arch_irq_disable();
    w->last = *(volatile uint64_t *)w->va;
    if (w->space) addr_space_switch(addr_space_kernel());
}

static int tlb_reads_past(struct tlb_watch *w, uint32_t n) {
    uint64_t end = timer_now_ns() + 500000000ull;
    while (__atomic_load_n(&w->reads, __ATOMIC_ACQUIRE) < n) {
        if (timer_now_ns() >= end) return 0;
        arch_cpu_relax();
    }
    return 1;
}

static void tlb_watch_end(unsigned i, struct tlb_watch *w) {
    w->stop = 1;
    (void)smp_call_wait(i, 1000000000ull);
}

/* A kernel page an AP is using, rewritten: first by hand with no
   shootdown, which the AP goes on missing (so this TLB does keep a stale
   entry, and the rest of the test means something), then by
   paging_map_kernel, after which the AP reads the new frame and has
   served the request. */
static int tlb_kernel_busy_steps(unsigned i, paddr_t a, paddr_t b) {
    struct cpu *c = cpu_at(i);
    paging_map_kernel(SCRATCH_VA, a, VM_WRITE);
    volatile uint64_t *e = entry_in(addr_space_kernel(), SCRATCH_VA);
    struct tlb_watch w = { .va = SCRATCH_VA };
    if (!e || smp_call_start(i, ap_watch, &w)) { paging_unmap(SCRATCH_VA); return 1; }
    int step = 0;
    if (!tlb_reads_past(&w, 1))     step = 2;
    else if (w.first != TLB_OLD)    step = 3;
    if (!step) {
        *e = pte64_encode(b, VM_WRITE, 0);
        uint32_t r = w.reads;
        if (!tlb_reads_past(&w, r + 1000)) step = 4;
        else if (w.last != TLB_OLD)        step = 5;   /* no stale entry: the test sees nothing */
    }
    if (step) { tlb_watch_end(i, &w); paging_unmap(SCRATCH_VA); return step; }
    uint32_t served = c->tlb_served;
    paging_map_kernel(SCRATCH_VA, b, VM_WRITE);   /* present: shot down */
    w.stop = 1;
    if (smp_call_wait(i, 1000000000ull)) step = 6;
    else if (w.last != TLB_NEW)          step = 7;
    else if (c->tlb_served == served)    step = 8;
    paging_unmap(SCRATCH_VA);
    return step;
}

static int t_tlb_kernel_busy(void) {
    for (unsigned i = 1; i < cpu_count(); i++) {
        paddr_t a, b;
        if (tlb_frames(&a, &b)) return smp_fail("tlb kernel busy: step ", i, 9);
        int step = tlb_kernel_busy_steps(i, a, b);
        pmm_free(a);
        pmm_free(b);
        if (step) return smp_fail("tlb kernel busy: step ", i, step);
    }
    return 1;
}

/* An idle AP is never interrupted for a shootdown: it is marked stale,
   and flushes as it wakes, before it reads anything. */
struct tlb_read { uintptr_t va; uint64_t seen; };

static void ap_read_once(void *arg) {
    struct tlb_read *r = arg;
    r->seen = *(volatile uint64_t *)r->va;
}

static int tlb_idle_steps(unsigned i, paddr_t a, paddr_t b) {
    struct cpu *c = cpu_at(i);
    paging_map_kernel(SCRATCH_VA, a, VM_WRITE);
    struct tlb_read r = { SCRATCH_VA, 0 };
    int step = 0;
    if (smp_call(i, ap_read_once, &r, 100000000ull) || r.seen != TLB_OLD) step = 1;
    uint64_t end = timer_now_ns() + 100000000ull;
    while (!step && !__atomic_load_n(&c->idle, __ATOMIC_ACQUIRE))
        if (timer_now_ns() >= end) step = 2;
    if (!step) {
        uint32_t irqs = c->irqs, served = c->tlb_served, woke = c->tlb_woke_stale;
        paging_map_kernel(SCRATCH_VA, b, VM_WRITE);   /* present, the AP idle */
        if (c->irqs != irqs || c->tlb_served != served || !c->tlb_stale) step = 3;
        else if (smp_call(i, ap_read_once, &r, 100000000ull)) step = 4;
        else if (r.seen != TLB_NEW)                         step = 5;
        else if (c->tlb_woke_stale != woke + 1 || c->irqs != irqs + 1) step = 6;
    }
    paging_unmap(SCRATCH_VA);
    return step;
}

static int t_tlb_idle(void) {
    for (unsigned i = 1; i < cpu_count(); i++) {
        paddr_t a, b;
        if (tlb_frames(&a, &b)) return smp_fail("tlb idle: step ", i, 7);
        int step = tlb_idle_steps(i, a, b);
        pmm_free(a);
        pmm_free(b);
        if (step) return smp_fail("tlb idle: step ", i, step);
    }
    return 1;
}

/* A user space's change goes to the CPU running it and to no other: AP 1
   reads a user page in a space of its own while AP 2 reads a kernel word,
   both busy; the page's frame is replaced. */
static volatile uint64_t tlb_quiet = TLB_OLD;

static int tlb_user_steps(paddr_t root, paddr_t a, paddr_t b) {
    struct cpu *in = cpu_at(1), *out = cpu_at(2);
    if (paging_map_user_in(root, 0x400000, a, 0)) return 1;
    struct tlb_watch win = { .va = 0x400000, .space = root };
    struct tlb_watch wout = { .va = (uintptr_t)&tlb_quiet };
    if (smp_call_start(1, ap_watch, &win)) return 2;
    if (smp_call_start(2, ap_watch, &wout)) { tlb_watch_end(1, &win); return 2; }
    int step = 0;
    if (!tlb_reads_past(&win, 1) || !tlb_reads_past(&wout, 1)) step = 3;
    else if (win.first != TLB_OLD || in->space != root)       step = 4;
    uint32_t sin = in->tlb_served, sout = out->tlb_served;
    if (!step && paging_map_user_in(root, 0x400000, b, 0)) step = 5;   /* present: shot down */
    tlb_watch_end(1, &win);
    tlb_watch_end(2, &wout);
    if (step) return step;
    if (win.last != TLB_NEW)       return 6;
    if (in->tlb_served == sin)     return 7;
    if (out->tlb_served != sout)   return 8;   /* not in the space: never asked */
    if (in->space != addr_space_kernel()) return 9;
    return 0;
}

static int t_tlb_user_targeted(void) {
    if (cpu_count() < 3) return 1;
    paddr_t a, b, root;
    if (tlb_frames(&a, &b)) return smp_fail("tlb user: step ", 0, 10);
    if (addr_space_create(&root)) { pmm_free(a); pmm_free(b); return smp_fail("tlb user: step ", 0, 10); }
    int step = tlb_user_steps(root, a, b);
    /* The frame the space maps goes back with it; the other is ours. */
    volatile uint64_t *e = entry_in(root, 0x400000);
    paddr_t kept = e && (*e & PTE64_PRESENT) ? (*e & PTE64_ADDR) : 0;
    addr_space_release(root);
    if (kept != a) pmm_free(a);
    if (kept != b) pmm_free(b);
    if (step) return smp_fail("tlb user: step ", 1, step);
    return 1;
}

/* No deadlock: two APs shooting down at each other at once, each waiting
   with interrupts off, both finish (and the boot CPU, asked by both,
   answers through the IPI); and an AP's shootdown aimed at the boot CPU
   while it spins, interrupts off, on a lock that only finishing it frees,
   finishes too. */
struct tlb_shoot {
    volatile uint32_t *go;
    spinlock_t *release;
    volatile uint32_t missing;
    volatile uint32_t *ready;        /* the crossfire: how many have arrived */
    uint32_t need;
};

/* For the crossfire, each AP turns interrupts off and waits for the other
   before it shoots: then neither can take the other's IPI, and only the
   serving in its own wait answers it. Without the barrier one AP woke from
   its halt after the other had finished, and nothing ever crossed. */
static void ap_shoot(void *arg) {
    struct tlb_shoot *s = arg;
    uint64_t end = arch_cycles64() + 2000000000ull;
    uint32_t irq = arch_irq_save();
    if (s->ready) {
        __atomic_fetch_add(s->ready, 1u, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(s->ready, __ATOMIC_ACQUIRE) < s->need && arch_cycles64() < end)
            arch_cpu_relax();
    } else {
        while (!*s->go && arch_cycles64() < end) arch_cpu_relax();
    }
    s->missing = smp_tlb_shootdown(SMP_TLB_KERNEL, SCRATCH_VA, 1, 300000000ull);
    arch_irq_restore(irq);
    if (s->release) __atomic_store_n(&s->release->locked, 0u, __ATOMIC_RELEASE);
}

static int t_tlb_no_deadlock(void) {
    if (cpu_count() < 2) return 1;
    volatile uint32_t go = 0;
    if (cpu_count() >= 3) {
        volatile uint32_t ready = 0;
        struct tlb_shoot s1 = { &go, 0, ~0u, &ready, 2 }, s2 = { &go, 0, ~0u, &ready, 2 };
        uint32_t boot = cpu_boot.tlb_served;
        if (smp_call_start(1, ap_shoot, &s1)) return smp_fail("tlb no deadlock: step ", 1, 1);
        if (smp_call_start(2, ap_shoot, &s2)) { go = 1; (void)smp_call_wait(1, 1000000000ull);
                                                return smp_fail("tlb no deadlock: step ", 2, 1); }
        go = 1;
        int w1 = smp_call_wait(1, 2000000000ull), w2 = smp_call_wait(2, 2000000000ull);
        if (w1 || w2) return smp_fail("tlb no deadlock: step ", w1 ? 1 : 2, 2);
        if (s1.missing || s2.missing) return smp_fail("tlb no deadlock: step ", s1.missing ? 1 : 2, 3);
        if (cpu_boot.tlb_served < boot + 2) return smp_fail("tlb no deadlock: step ", 0, 4);
    }
    spinlock_t held = SPINLOCK_INIT(LR_PAGEDB);
    held.locked = 1;                 /* taken for the AP, which frees it once its shootdown ends */
    struct tlb_shoot s = { &go, &held, ~0u, 0, 0 };
    go = 1;
    /* Interrupts off from before the call: the IPI cannot be taken, and
       only the spin's serving answers it. */
    uint32_t irq = arch_irq_save();
    if (smp_call_start(1, ap_shoot, &s)) { arch_irq_restore(irq); return smp_fail("tlb no deadlock: step ", 1, 5); }
    uint32_t f = spin_lock(&held);   /* spins, serving */
    spin_unlock(&held, f);
    arch_irq_restore(irq);
    if (smp_call_wait(1, 1000000000ull)) return smp_fail("tlb no deadlock: step ", 1, 6);
    if (s.missing) return smp_fail("tlb no deadlock: step ", 1, 7);
    return 1;
}

/* The boot CPU polls hardware with interrupts off, as a driver's bounded
   wait does inside a syscall: an AP's shootdown is answered by the poll
   (timer_timeout_expired serves), not left for its timeout. */
static int t_tlb_poll_serves(void) {
    if (cpu_count() < 2) return 1;
    volatile uint32_t go = 1;
    struct tlb_shoot s = { &go, 0, ~0u, 0, 0 };
    uint32_t served = cpu_boot.tlb_served;
    uint32_t irq = arch_irq_save();
    if (smp_call_start(1, ap_shoot, &s)) { arch_irq_restore(irq); return smp_fail("tlb poll: step ", 1, 1); }
    struct timeout to;
    timer_timeout_start(&to, 2000);              /* interrupts off: the spin budget */
    while (s.missing == ~0u && !timer_timeout_expired(&to)) { }
    arch_irq_restore(irq);
    if (smp_call_wait(1, 1000000000ull)) return smp_fail("tlb poll: step ", 1, 2);
    if (s.missing) return smp_fail("tlb poll: step ", 1, 3);
    if (cpu_boot.tlb_served == served) return smp_fail("tlb poll: step ", 0, 4);
    return 1;
}

/* The boot CPU raises a signature to its exponent with interrupts off, as
   an image's check does inside a syscall: the remainders serve an AP's
   shootdown (lib/crypto/bignum.c), which times out long before they end. */
static int t_tlb_modexp_serves(void) {
    if (cpu_count() < 2) return 1;
    static bignum n, b, m;
    uint8_t nb[256], sb[256];
    for (int i = 0; i < 256; i++) { nb[i] = 0xFF; sb[i] = (uint8_t)(i * 7 + 1); }
    bn_from_bytes(&n, nb, sizeof nb);
    bn_from_bytes(&b, sb, sizeof sb);
    volatile uint32_t go = 1;
    struct tlb_shoot s = { &go, 0, ~0u, 0, 0 };
    uint32_t served = cpu_boot.tlb_served;
    uint32_t irq = arch_irq_save();
    if (smp_call_start(1, ap_shoot, &s)) { arch_irq_restore(irq); return smp_fail("tlb modexp: step ", 1, 1); }
    /* Raised again until the shootdown's 300 ms have passed with interrupts
       off: one is that long under icount, a few ms on Hyper-V (1.9f). */
    uint64_t t0 = timer_now_ns(), took;
    unsigned runs = 0;
    do {
        bn_modexp(&m, &b, RSA_PROFILE_EXPONENT, &n);
        took = timer_now_ns() - t0;
    } while (took < 300000000ull && ++runs < 1000u);
    arch_irq_restore(irq);
    if (smp_call_wait(1, 1000000000ull)) return smp_fail("tlb modexp: step ", 1, 2);
    if (s.missing) return smp_fail("tlb modexp: step ", 1, 3);
    if (cpu_boot.tlb_served == served) return smp_fail("tlb modexp: step ", 0, 4);
    /* Shorter than the shootdown's 300 ms, and the test could not tell. */
    if (took < 300000000ull) return smp_fail("tlb modexp: step ", 0, 5);
    return 1;
}

/* ---- the scheduler on every CPU (1.9e2, cpu/sched.c) ----
 * Threads made here run on an AP by thread_create_on; each test waits for
 * its own to be reaped, which the boot CPU does, before it returns. */

static int smp_reaped(uint32_t n0) {
    for (int i = 0; i < 3000; i++) {
        if (sched_thread_count() == n0) return 1;
        /* A failed test's thread may be ready on an AP left halted, never
           asked: a call ends the halt, and its idle thread picks it then.
           So a failure leaves nothing for the tests after it. */
        if (i % 100 == 99)
            for (unsigned c = 1; c < cpu_count(); c++) (void)smp_call(c, ap_nothing, 0, 10000000ull);
        thread_sleep_ms(1);
    }
    return 0;
}

/* A wait of the boot CPU's that asks the event timer for nothing: no sleep
   of its own arms a deadline that would hide one an AP's never reached. */
static int spin_until(volatile uint32_t *flag, uint32_t want, uint64_t ms) {
    uint64_t end = timer_now_ns() + ms * TIMER_NS_PER_TICK;
    while (*flag != want && timer_now_ns() < end) arch_cpu_relax();
    return *flag == want;
}

/* Each CPU has its idle thread, outside the id table and the count; an AP
   with nothing to run runs it, and the boot CPU runs this thread. */
static int t_sched_idle_threads(void) {
    uint32_t n0 = sched_thread_count();
    for (unsigned i = 0; i < cpu_count(); i++) {
        struct cpu *c = cpu_at(i);
        struct thread *t = c->idle_thread;
        if (!t || !t->idle || t->affinity != 1u << i || t->id != THREAD_IDLE_ID + (int)i)
            return smp_fail("sched idle: step ", i, 1);
        if (thread_is_alive(t->id) || thread_cpu(t->id) != -1 || thread_set_affinity(t->id, 1u) != -1)
            return smp_fail("sched idle: step ", i, 2);
        if (i && (c->cur != t || t->on_cpu != i + 1 || c->thread != t->id))
            return smp_fail("sched idle: step ", i, 3);
    }
    int me = thread_current_id();
    if (cpu_boot.cur->id != me || thread_cpu(me) != 0 || cpu_boot.idle_thread->on_cpu)
        return smp_fail("sched idle: step ", 0, 4);
    if (sched_thread_count() != n0) return smp_fail("sched idle: step ", 0, 5);
    return 1;
}

static volatile uint32_t where_cpu;
static void where_thread(void) { where_cpu = cpu_this()->index + 1; }

static uint32_t run_where(uint32_t mask) {
    where_cpu = 0;
    int id = mask ? thread_create_on("where", where_thread, mask) : thread_create("where", where_thread);
    if (id < 0) return 0;
    /* This thread keeps the boot CPU for a while first: only an AP could
       run the new one meanwhile, which a thread made as before may not. */
    if (!mask) {
        uint64_t end = timer_now_ns() + 20 * TIMER_NS_PER_TICK;
        while (!where_cpu && timer_now_ns() < end) arch_cpu_relax();
    }
    for (int k = 0; k < 500 && !where_cpu; k++) thread_sleep_ms(1);
    return where_cpu;
}

/* A thread made as before stays on the boot CPU; one made for an AP runs
   there; one for no CPU there is is refused and leaves nothing. */
static int t_sched_placement(void) {
    uint32_t n0 = sched_thread_count();
    unsigned cpu = 0;
    int step = 0;
    if (run_where(0) != 1) step = 1;
    for (unsigned i = 1; i < cpu_count() && !step; i++)
        if (run_where(1u << i) != i + 1) { cpu = i; step = 2; }
    if (!step && cpu_count() < 32 && thread_create_on("none", where_thread, ~0u << cpu_count()) != -1)
        step = 3;
    if (!smp_reaped(n0) && !step) step = 4;
    return step ? smp_fail("sched placement: step ", cpu, step) : 1;
}

/* A thread blocked on an idle AP, woken from the boot CPU: the AP's halt
   ends by the wake's IPI, not by a timer, which an idle AP has none of. */
static volatile uint32_t wk_state;
static void wake_thread(void) {
    wk_state = 1;
    thread_wait();
    wk_state = 2;
}

static int t_sched_wake_ipi(void) {
    if (cpu_count() < 2) return 1;
    uint32_t n0 = sched_thread_count();
    unsigned i = cpu_count() - 1;
    wk_state = 0;
    int id = thread_create_on("wake", wake_thread, 1u << i);
    if (id < 0) return smp_fail("sched wake: step ", i, 1);
    for (int k = 0; k < 500 && !thread_is_blocked(id); k++) thread_sleep_ms(1);
    if (!thread_is_blocked(id) || wk_state != 1) return smp_fail("sched wake: step ", i, 2);
    /* BLOCKED is set before the AP switches away: wait for its halt, or the
       wake can land while it is still on its way there and end no halt
       (seen once 1.9f's counted lock lengthened that way). */
    for (int k = 0; k < 500 && !__atomic_load_n(&cpu_at(i)->idle, __ATOMIC_RELAXED); k++) thread_sleep_ms(1);
    if (!__atomic_load_n(&cpu_at(i)->idle, __ATOMIC_RELAXED)) return smp_fail("sched wake: step ", i, 6);
    uint32_t ipis = cpu_at(i)->resched_ipis, wakes = cpu_at(i)->wakes;
    thread_unblock(id);
    int woke = spin_until(&wk_state, 2, 100);
    if (!woke) { (void)smp_reaped(n0); return smp_fail("sched wake: step ", i, 3); }
    if (cpu_at(i)->resched_ipis == ipis || cpu_at(i)->wakes == wakes)
        return smp_fail("sched wake: step ", i, 4);
    if (!smp_reaped(n0)) return smp_fail("sched wake: step ", i, 5);
    return 1;
}

/* A sleep on an AP ends at its time: the deadline is the boot CPU's event
   timer's, reached from the AP by the hint it forwards. The boot CPU spins
   meanwhile, so nothing of its own sets the timer sooner. */
static volatile uint32_t sl_done;
static volatile uint64_t sl_took;
static void ap_sleeper(void) {
    uint64_t t0 = timer_now_ns();
    thread_sleep_ms(20);
    sl_took = timer_now_ns() - t0;
    sl_done = 1;
}

static int t_sched_ap_sleep(void) {
    if (cpu_count() < 2 || timer_source() != TIMER_SOURCE_APIC) return 1;
    uint32_t n0 = sched_thread_count();
    sl_done = 0; sl_took = 0;
    if (thread_create_on("ap-sleep", ap_sleeper, 1u << 1) < 0) return smp_fail("sched ap sleep: step ", 1, 1);
    if (!spin_until(&sl_done, 1, 500)) { (void)smp_reaped(n0); return smp_fail("sched ap sleep: step ", 1, 2); }
    if (sl_took < 20 * TIMER_NS_PER_TICK || sl_took > 80 * TIMER_NS_PER_TICK) {
        klog_child_u32("slept ms", (uint32_t)(sl_took / TIMER_NS_PER_TICK), LOG_COLOR_VALUE, "");
        (void)smp_reaped(n0);
        return smp_fail("sched ap sleep: step ", 1, 3);
    }
    if (!smp_reaped(n0)) return smp_fail("sched ap sleep: step ", 1, 4);
    return 1;
}

/* Two threads on one AP that never yield: the second runs only because the
   AP's own timer ends the first one's slice. */
static volatile uint32_t sp_b_ran, sp_a_saw;
static void spin_a(void) {
    uint64_t end = arch_cycles64() + 1000000000ull;
    while (!sp_b_ran && arch_cycles64() < end) arch_cpu_relax();
    sp_a_saw = sp_b_ran ? 1 : 2;
}
static void spin_b(void) { sp_b_ran = 1; }

static int t_sched_ap_slice(void) {
    if (cpu_count() < 2 || timer_source() != TIMER_SOURCE_APIC) return 1;
    uint32_t n0 = sched_thread_count();
    unsigned i = 1;
    sp_b_ran = sp_a_saw = 0;
    sched_preempt_enable(5);
    int a = thread_create_on("spin-a", spin_a, 1u << i);
    int b = a < 0 ? -1 : thread_create_on("spin-b", spin_b, 1u << i);
    int ok = b >= 0 && spin_until(&sp_a_saw, 1, 1500);
    sched_preempt_disable();
    int reaped = smp_reaped(n0);
    if (!ok) return smp_fail("sched ap slice: step ", i, 1);
    if (!reaped) return smp_fail("sched ap slice: step ", i, 2);
    return 1;
}

/* A thread raised above the one an AP is running takes the AP at once, by
   the IPI that asks it to switch: the slice is a second, so it is not the
   slice's end that does it. */
static volatile uint32_t hi_ran, lo_saw;
static void busy_lo(void) {
    uint64_t end = arch_cycles64() + 1000000000ull;
    while (!hi_ran && arch_cycles64() < end) arch_cpu_relax();
    lo_saw = hi_ran ? 1 : 2;
}
static void busy_hi(void) { hi_ran = cpu_this()->index + 1; }

static int t_sched_outrank_ap(void) {
    if (cpu_count() < 2) return 1;
    uint32_t n0 = sched_thread_count();
    unsigned i = 1;
    hi_ran = lo_saw = 0;
    sched_preempt_enable(1000);
    int lo = thread_create_on("busy-lo", busy_lo, 1u << i);
    int hi = lo < 0 ? -1 : thread_create_on("busy-hi", busy_hi, 1u << i);
    int ok = 0;
    if (hi >= 0) {
        thread_sleep_ms(5);                       /* busy-lo is running, busy-hi waits */
        ok = thread_cpu(lo) == (int)i && !hi_ran;
        thread_set_prio(hi, PRIO_NORMAL + 2);
        ok = ok && spin_until(&lo_saw, 1, 100) && hi_ran == i + 1;
    }
    sched_preempt_disable();
    int reaped = smp_reaped(n0);
    if (!ok) return smp_fail("sched outrank: step ", i, 1);
    if (!reaped) return smp_fail("sched outrank: step ", i, 2);
    return 1;
}

/* A thread woken between blocking and switching out is ready while still
   on its CPU: no other CPU it may use takes it until it is off. Here it may
   use AP 1 and AP 2; blocked on AP 1, it spins there while the boot CPU
   wakes it and has AP 2 look again. AP 2 taking it would start it a second
   time, from the frame it was made with. */
static volatile uint32_t tw_entered, tw_prepared, tw_go;
static void two_cpu_thread(void) {
    if (__atomic_add_fetch(&tw_entered, 1u, __ATOMIC_SEQ_CST) > 1) return;
    uint32_t irq = arch_irq_save();
    thread_block_prepare(0);
    tw_prepared = 1;
    uint64_t end = arch_cycles64() + 1000000000ull;
    while (!tw_go && arch_cycles64() < end) arch_cpu_relax();
    /* Not woken (the test failed before its wake): it wakes itself, so it
       ends rather than wait for the tests after it. */
    if (thread_is_blocked(thread_current_id())) thread_unblock(thread_current_id());
    arch_irq_restore(irq);
    thread_block_finish();
}

static int t_sched_ready_on_cpu(void) {
    if (cpu_count() < 3) return 1;
    uint32_t n0 = sched_thread_count();
    tw_entered = tw_prepared = tw_go = 0;
    int id = thread_create_on("two-cpu", two_cpu_thread, (1u << 1) | (1u << 2));
    int step = 0;
    if (id < 0) step = 1;
    else if (!spin_until(&tw_prepared, 1, 500) || thread_cpu(id) != 1) step = 2;
    else {
        thread_unblock(id);
        (void)smp_call(2, ap_nothing, 0, 100000000ull);
        uint64_t end = timer_now_ns() + 10 * TIMER_NS_PER_TICK;
        while (timer_now_ns() < end) arch_cpu_relax();
        if (tw_entered != 1 || thread_cpu(id) != 1) step = 3;
    }
    tw_go = 1;
    int reaped = smp_reaped(n0);
    if (step) return smp_fail("sched ready on cpu: step ", 2, step);
    if (!reaped) return smp_fail("sched ready on cpu: step ", 2, 4);
    return 1;
}

/* A running thread's affinity changed away from its CPU: it leaves at that
   CPU's next preemption point and is handed to the one it may use. */
static volatile uint32_t mv_stop, mv_last;
static void mover(void) {
    uint64_t end = arch_cycles64() + 1000000000ull;
    while (!mv_stop && arch_cycles64() < end) mv_last = cpu_this()->index + 1;
}

static int t_sched_affinity_moves(void) {
    if (cpu_count() < 3) return 1;
    uint32_t n0 = sched_thread_count();
    mv_stop = mv_last = 0;
    sched_preempt_enable(1000);
    int id = thread_create_on("mover", mover, 1u << 1);
    int ok = 0;
    if (id >= 0) {
        ok = spin_until(&mv_last, 2, 100);
        ok = ok && thread_affinity(id) == 1u << 1 && thread_set_affinity(id, 1u << 2) == 0;
        ok = ok && spin_until(&mv_last, 3, 100) && thread_affinity(id) == 1u << 2;
        mv_stop = 1;
    }
    sched_preempt_disable();
    int reaped = smp_reaped(n0);
    if (!ok) return smp_fail("sched affinity: step ", 1, 1);
    if (!reaped) return smp_fail("sched affinity: step ", 1, 2);
    return 1;
}

/* ---- the locks of 1.9e3 ----
 * Each held here, on the boot CPU, while a thread on an AP calls what takes
 * it: the thread is still in the call after a while, and finishes once the
 * lock is given back (the 1.5b2 rule). The boot CPU spins meanwhile
 * (spin_until): a sleep would let the thread run here instead. */
static volatile uint32_t lk_done, lk_go;
static void lk_storage_call(void) { (void)cxfs_get_id(); }
static void lk_log_call(void)     { slog_record("KTEST", "", SEV_INFO, "the log lock, from an AP"); }
static void lk_files_call(void)   { (void)sysfile_lock_waiting(); }

/* Made before the lock is held (making a thread takes locks ranked before
   the log's), running before it is asked: 1 running, 2 through the call. */
static void (*volatile lk_call)(void);
static void lk_thread(void) {
    lk_done = 1;
    while (!lk_go) arch_cpu_relax();
    lk_call();
    lk_done = 2;
}

static void lk_storage_hold(void) { storage_lock(); }
static void lk_storage_give(void) { storage_unlock(); }
static void lk_log_hold(void)     { log_lock(); }
static void lk_log_give(void)     { log_unlock(); }
static uint32_t lk_files_flags;
static void lk_files_hold(void)   { lk_files_flags = sysfile_test_hold(); }
static void lk_files_give(void)   { sysfile_test_release(lk_files_flags); }

/* 0, or the step that failed: 1 the thread got through the held lock, 2 it
   never finished once the lock was given back, 3 it was never reaped. */
static int lk_blocks(void (*hold)(void), void (*give)(void), void (*call)(void)) {
    uint32_t n0 = sched_thread_count();
    lk_done = lk_go = 0;
    lk_call = call;
    int id = thread_create_on("lock-ap", lk_thread, 1u << 1);
    int step = id < 0 || !spin_until(&lk_done, 1, 1000) ? 2 : 0;
    if (!step) {
        hold();
        lk_go = 1;
        if (spin_until(&lk_done, 2, 30)) step = 1;
        give();
    }
    lk_go = 1;
    if (!step && !spin_until(&lk_done, 2, 1000)) step = 2;
    if (!smp_reaped(n0) && !step) step = 3;
    return step;
}

static int t_storage_lock(void) {
    if (cpu_count() < 2) return 1;
    int step = lk_blocks(lk_storage_hold, lk_storage_give, lk_storage_call);
    if (step) return smp_fail("storage lock: step ", 1, step);
    /* CPU-recursive: held through an inner take and give, until the outer */
    storage_lock();
    storage_lock();
    int held2 = storage_held();
    storage_unlock();
    int held1 = storage_held();
    storage_unlock();
    if (!held2 || !held1 || storage_held()) return smp_fail("storage lock: step ", 0, 4);
#if CXK_LOCK_CHECK
    /* Only the outer take is in the lock order: under a test lock of its
       rank, one CXFS call is one failure, the inner takes none. */
    static spinlock_t lk_rank = SPINLOCK_INIT(LR_STORAGE);
    uint32_t n = 0;
    lockorder_test_arm();
    uint32_t f = spin_lock(&lk_rank);
    storage_lock();
    (void)cxfs_get_id();
    storage_unlock();
    spin_unlock(&lk_rank, f);
    uint32_t code = lockorder_test_disarm(&n);
    if (code != LO_ORDER || n != 1) return smp_fail("storage lock: step ", 0, 5);
#endif
    return 1;
}

/* The log lock, then the ring with two CPUs recording at once: every
   record whole, its sequence number the next, each CPU's in its order. */
#define LK_LINES 200u
static void lk_ring_line(char tag, uint32_t i) {
    char ev[16];
    ev[0] = tag;
    ev[1 + fmt_u32(ev + 1, i)] = 0;
    slog_record("KTEST", "ring", SEV_INFO, ev);
}
static void lk_ring_thread(void) {
    while (!lk_go) arch_cpu_relax();
    for (uint32_t i = 0; i < LK_LINES; i++) lk_ring_line('a', i);
    lk_done = 1;
}

static int t_log_lock(void) {
    if (cpu_count() < 2) return 1;
    int step = lk_blocks(lk_log_hold, lk_log_give, lk_log_call);
    if (step) return smp_fail("log lock: step ", 1, step);

    uint32_t n0 = sched_thread_count(), t0 = slog_total();
    lk_done = lk_go = 0;
    if (thread_create_on("ring-ap", lk_ring_thread, 1u << 1) < 0) return smp_fail("log lock: step ", 1, 4);
    lk_go = 1;
    for (uint32_t i = 0; i < LK_LINES; i++) lk_ring_line('b', i);
    int done = spin_until(&lk_done, 1, 2000);
    if (!smp_reaped(n0) || !done) return smp_fail("log lock: step ", 1, 5);
    if (slog_total() - t0 < 2 * LK_LINES) return smp_fail("log lock: step ", 0, 6);
    /* Ours are the ring's, past t0, among whatever else was recorded */
    uint32_t next[2] = { 0, 0 }, n = slog_count(), first = n - (slog_total() - t0);
    for (uint32_t i = first; i < n; i++) {
        const struct slog_entry *e = slog_get(i);
        if (!e || e->seq != t0 + (i - first)) return smp_fail("log lock: step ", 0, 7);
        if (strcmp(e->subsystem, "ring") != 0) continue;
        int w = e->event[0] == 'a' ? 0 : e->event[0] == 'b' ? 1 : -1;
        char want[16];
        want[0] = e->event[0];
        if (w >= 0) want[1 + fmt_u32(want + 1, next[w])] = 0;
        if (w < 0 || strcmp(e->event, want) != 0 || strcmp(e->system, "KTEST") != 0)
            return smp_fail("log lock: step ", 0, 8);
        next[w]++;
    }
    if (next[0] != LK_LINES || next[1] != LK_LINES) return smp_fail("log lock: step ", 0, 9);
    return 1;
}

static int t_files_lock(void) {
    if (cpu_count() < 2) return 1;
    int step = lk_blocks(lk_files_hold, lk_files_give, lk_files_call);
    if (step) return smp_fail("files lock: step ", 1, step);
    return 1;
}

/* The wall clock's CMOS reads take their lock: under a test lock of its
   rank, one read is one failure. */
static int t_cmos_lock(void) {
#if CXK_LOCK_CHECK
    static spinlock_t lk_rank = SPINLOCK_INIT(LR_CMOS);
    struct datetime d;
    uint32_t n = 0;
    lockorder_test_arm();
    uint32_t f = spin_lock(&lk_rank);
    int rc = platform_wallclock(&d);
    spin_unlock(&lk_rank, f);
    uint32_t code = lockorder_test_disarm(&n);
    if (rc != 0 || code != LO_ORDER || n != 1) return smp_fail("cmos lock: step ", 0, 1);
    return 1;
#else
    return 0;   /* no checker to count with: fails, never skips */
#endif
}

/* The APs reap (1.9e3): a thread ends on AP 1 while the boot CPU spins
   with interrupts off, so it never switches and reaps nothing; an AP's idle
   thread does, AP 1's or another woken meanwhile. */
static uint32_t lk_ap_reaps(void) {
    uint32_t n = 0;
    for (unsigned i = 1; i < cpu_count(); i++) n += sched_reaped_on(i);
    return n;
}

static void lk_short_thread(void) { lk_done = 1; }

static int t_reap_on_ap(void) {
    if (cpu_count() < 2) return 1;
    uint32_t n0 = sched_thread_count(), r0 = lk_ap_reaps(), b0 = sched_reaped_on(0);
    lk_done = 0;
    uint32_t f = arch_irq_save();
    int id = thread_create_on("reap-ap", lk_short_thread, 1u << 1);
    int gone = 0;
    uint64_t end = timer_now_ns() + 1000ull * 1000000u;
    while (id >= 0 && timer_now_ns() < end && !(gone = sched_thread_count() == n0)) {
        smp_serve();
        arch_cpu_relax();
    }
    uint32_t b1 = sched_reaped_on(0);
    arch_irq_restore(f);
    int reaped = smp_reaped(n0);
    if (id < 0 || !lk_done) return smp_fail("reap on ap: step ", 1, 1);
    if (!gone) return smp_fail("reap on ap: step ", 1, 2);
    if (lk_ap_reaps() == r0 || b1 != b0) return smp_fail("reap on ap: step ", 1, 3);
    if (!reaped) return smp_fail("reap on ap: step ", 1, 4);
    return 1;
}

/* ---- arch/ktest_arch.h ---- */

/* The double fault's IST stack (t_idt checks the gate names it): the wiring
   ktest_run asks every architecture about, by taking one. */

/* ---- the locks of 1.9e4 ----
 * As 1.9e3's: each held on the boot CPU while an AP's thread calls what
 * takes it. The input buffer's, the jobs', the processes'; the log lock over
 * a process's output and the framebuffer's syscalls, and the storage lock
 * over a font's selection, which share what the console draws with. */
static void lk_input_call(void) { keyboard_inject('q'); }
/* A killed job's charge: refused, under the lock, from the check on. */
static struct job *lk_job;
static volatile int lk_job_rc;
static void lk_job_call(void)   { lk_job_rc = job_charge_thread(lk_job); }
static void lk_proc_call(void)  { (void)proc_stops_armed(); }
static struct proc lk_out_proc;   /* only its output buffer is used */
static void lk_out_call(void)   { proc_out_write(&lk_out_proc, "x", 1); }
static void lk_pixel_call(void) { (void)sys_fb_put_pixel(0, 0, 0); }
/* A name too long to be one: refused before any read, which would take the
   storage lock by itself, so only the lock taken first holds the call. */
static void lk_font_call(void)  {
    (void)fb_font_select("ktest-a-name-far-too-long-to-be-any-font-on-cxfs-at-all-xxxxxxxx");
}
static uint32_t lk_input_flags, lk_job_flags, lk_proc_flags;
static void lk_input_hold(void) { lk_input_flags = keyboard_test_hold(); }
static void lk_input_give(void) { keyboard_test_release(lk_input_flags); }
static void lk_job_hold(void)   { lk_job_flags = job_test_hold(); }
static void lk_job_give(void)   { job_test_release(lk_job_flags); }
static void lk_proc_hold(void)  { lk_proc_flags = proc_test_hold(); }
static void lk_proc_give(void)  { proc_test_release(lk_proc_flags); }

static int t_input_lock(void) {
    if (cpu_count() < 2) return 1;
    int step = lk_blocks(lk_input_hold, lk_input_give, lk_input_call);
    if (step) return smp_fail("input lock: step ", 1, step);
    /* and the key went in, whole, once */
    if (keyboard_getchar() != 'q' || keyboard_getchar() != 0) return smp_fail("input lock: step ", 0, 4);
    return 1;
}

static int t_job_proc_locks(void) {
    if (cpu_count() < 2) return 1;
    int rc;
    lk_job = job_create(job_root(), 0, 0, 0, &rc);
    if (!lk_job) return smp_fail("jobs lock: step ", 0, 1);
    job_kill(lk_job, 0);
    lk_job_rc = E_OK;
    int step = lk_blocks(lk_job_hold, lk_job_give, lk_job_call);
    if (!step && lk_job_rc != E_CANCELED) step = 4;
    job_put(lk_job);
    if (step) return smp_fail("jobs lock: step ", 1, step);
    step = lk_blocks(lk_proc_hold, lk_proc_give, lk_proc_call);
    if (step) return smp_fail("proc lock: step ", 1, step + 4);
    return 1;
}

static int t_log_lock_draws(void) {
    if (cpu_count() < 2) return 1;
    lk_out_proc.outlen = 0;
    int step = lk_blocks(lk_log_hold, lk_log_give, lk_out_call);
    if (!step && lk_out_proc.outlen != 1) step = 4;
    lk_out_proc.outlen = 0;
    if (step) return smp_fail("process output: step ", 1, step);
    /* No framebuffer, no drawing: the syscall answers before the lock. */
    if (fb_active() && (step = lk_blocks(lk_log_hold, lk_log_give, lk_pixel_call)) != 0)
        return smp_fail("fb draw: step ", 1, step);
    step = lk_blocks(lk_storage_hold, lk_storage_give, lk_font_call);
    if (step) return smp_fail("font select: step ", 1, step);
    return 1;
}

/* The last of a process's threads out, decided under the lock that counts
 * them: two threads of one process stop at once on two APs, each with its
 * own exit code, released together from behind proc_lock. Exactly one is
 * last, and the process ends EXITED with that one's code. Decided before
 * the lock (as thread_exit did until 1.9e4), each saw the other still
 * running and neither noted its code. */
static struct proc *lx_proc;
static volatile uint32_t lx_ready, lx_go, lx_done;
static void lx_stop(int32_t code) {
    __atomic_add_fetch(&lx_ready, 1u, __ATOMIC_SEQ_CST);
    while (!lx_go) arch_cpu_relax();
    proc_thread_stopped(lx_proc, &code);
    __atomic_add_fetch(&lx_done, 1u, __ATOMIC_SEQ_CST);
}
static void lx_a(void) { lx_stop(11); }
static void lx_b(void) { lx_stop(22); }

static int t_proc_last_exit(void) {
    if (cpu_count() < 3) return 1;
    uint32_t n0 = sched_thread_count(), live0 = proc_live();
    struct proc *p = proc_create(0, job_root());
    if (!p) return smp_fail("last exit: step ", 0, 1);
    int in = proc_thread_in(p) == E_OK;
    in = proc_thread_in(p) == E_OK && in;
    lx_proc = p;
    lx_ready = lx_go = lx_done = 0;
    int step = in ? 0 : 1;
    if (!step && (thread_create_on("lastexit-a", lx_a, 1u << 1) < 0 ||
                  thread_create_on("lastexit-b", lx_b, 1u << 2) < 0 ||
                  !spin_until(&lx_ready, 2, 1000))) step = 2;
    if (!step) {
        lk_proc_hold();
        lx_go = 1;
        if (spin_until(&lx_done, 1, 30)) step = 3;   /* through a held proc_lock */
        lk_proc_give();
    }
    lx_go = 1;
    if (!step && !spin_until(&lx_done, 2, 1000)) step = 4;
    if (!step && (p->ended != PROC_EXITED || (p->end_code != 11 && p->end_code != 22))) step = 5;
    if (!step && !(atomic_load(&p->hdr.signals) & SIGNAL_PROCESS_ENDED)) step = 6;
    /* Gone only once both are through: a thread still in the call leaks the
       record rather than freeing it under it (the 1.3d rule). */
    if (lx_done == 2) {
        proc_thread_gone(p);
        proc_thread_gone(p);
        proc_put(p);
        if (!step && !proc_live_settles(live0)) step = 7;
    }
    if (!smp_reaped(n0) && !step) step = 8;
    if (step) return smp_fail("last exit: step ", 1, step);
    return 1;
}

/* A thread made to run ring-3 code may run on any CPU; a kernel thread
 * still only on the boot CPU (t_sched_placement). */
static void lx_never(void) { }
static int t_user_threads_any_cpu(void) {
    if (cpu_count() < 2) return 1;
    uint32_t online = cpu_count() >= 32 ? 0xFFFFFFFFu : (1u << cpu_count()) - 1u;
    uint32_t live0 = proc_live();
    int u = thread_create_user("anycpu-user", lx_never);
    int step = u < 0 ? 1 : thread_affinity(u) != online ? 2 : 0;
    if (u >= 0) thread_discard(u);
    int q = thread_create_process("anycpu-proc", lx_never, job_root());
    if (!step) step = q < 0 ? 3 : thread_affinity(q) != online ? 4 : 0;
    if (q >= 0) thread_discard(q);
    if (!step && !proc_live_settles(live0)) step = 5;
    if (step) return smp_fail("user thread's CPUs: step ", 0, step);
    return 1;
}

int ktest_arch_double_fault_stack(void) {
    return t_double_fault_ist();
}

/* What x86-32's asks, in 64-bit terms: a syscall gives ring 3 back its own
   segment registers. sysretq loads CS and SS from STAR; the data registers it
   leaves as the entry found them, so the blob compares them across a call
   that switches away (usermode.asm, user64_segs). */
extern uint8_t user64_segs_start[], user64_segs_end[];

int ktest_arch_user_segments_kept(void) {
    int rc = usermode_run_blob(user64_segs_start,
                               (uint32_t)(user64_segs_end - user64_segs_start));
    return rc == (int)(USER_CODE_SEL | USER_DATA_SEL << 8);
}

/* In the order they were written: the CPU's tables, paging and the page
   allocator (5b, 5c), then the heap, interrupts, the tick and threads (5d),
   then ring 3 (5e). */
static const struct ktest_arch_case cases[] = {
    { "long mode is active (EFER.LMA)",                 t_long_mode },
    { "no-execute is enabled (EFER.NXE)",               t_nx },
    { "the kernel runs in the top 2 GiB",               t_top_2g },
    { "the loader handed over CXBI",                    t_cxbi },
    { "the GDT is loaded, with ring-3 code and data",   t_gdt },
    { "the TSS is loaded and busy",                     t_tss },
    { "the IDT is loaded; #DF, NMI, #DB, #MC on ISTs",  t_idt },
    { "#BP returns through the IDT after int3",         t_breakpoint },
    { "#UD reports the faulting RIP",                   t_invalid_opcode },
    { "#GP carries the selector as its error code",     t_general_protection },
    { "a double fault runs on its IST stack",           t_double_fault_ist },
    { "the kernel runs on its own page tables, CR0.WP set", t_own_tables },
    { "all 256 kernel PML4 entries exist; user half empty", t_kernel_half },
    { "the loader's maps are gone; a null read faults",  t_loader_maps_gone },
    { "kernel text is read-only",                       t_text_readonly },
    { "kernel rodata is read-only",                     t_rodata_readonly },
    { "kernel data and stack are no-execute",           t_data_stack_nx },
    { "the direct map covers RAM and nothing else",     t_direct_map_covers_ram },
    { "the direct map shows each frame's own bytes",    t_direct_map_is_physical },
    { "the direct map is no-execute",                   t_direct_map_nx },
    { "the page allocator hands out only free RAM",     t_pmm_hands_out_only_ram },
    { "a double free is refused and changes nothing",   t_pmm_double_free },
    { "a run is contiguous, aligned, and costs its size", t_pmm_runs },
    { "pages above 4 GB first; below 4 GB on request",  t_pmm_zones },
    { "a user mapping cannot name the kernel half",     t_user_map_refuses_kernel },
    { "a new address space shares the kernel, frees all", t_address_space },
    { "device registers map once, uncached, and answer", t_map_phys },
    { "PAT: entry 4 write-combining, entries 0-3 as at power-on", t_pat },
    { "a VM_WC page is PAT entry 4; the screen is mapped so", t_wc_pages },
    { "the user half's last page is never mapped",     t_user_top_page },
    { "kmalloc: small from the caches, large from the list", t_heap },
    { "thread 0 runs on a guarded kernel stack",        t_main_stack_guarded },
    { "the tick's source interrupts with interrupts on", t_ticks },
    { "the I/O APIC routes the PIT's tick; the 8259 is masked", t_io_apic },
    { "100 ticks are 100 ms of the ACPI PM timer",      t_tick_rate },
    { "a 50 ms sleep is 50 to 100 ticks",               t_sleep },
    { "on the PIT fallback too",                        t_pit_sleep },
    { "the clock is the TSC: finer than a tick, never back", t_clock_tsc },
    { "a TSC rate Hyper-V states is the clock's",       t_stated_tsc_rate },
    { "the local APIC timer is the source; the PIT is masked", t_apic_source },
    { "an idle 100 ms sleep is a few interrupts, not 100", t_idle_quiet },
    { "deadlines are met at their time, not at a tick", t_deadline_precise },
    { "slack: a timer fires at a later one's interrupt",    t_slack },
    { "two threads that yield both run to the end",     t_coop },
    { "a switch keeps rbx and r12-r15",                 t_switch_keeps_registers },
    { "a new thread starts with interrupts on",         t_new_thread_irqs_on },
    { "the tick preempts threads that never yield",     t_preempt },
    { "RSP0 follows the running thread",                t_rsp0_follows_thread },
    { "FS_BASE follows the running thread",             t_tls_follows_thread },
    { "GS is this CPU's; the IST vectors swap it in",   t_cpu_local },
    { "every enabled MADT CPU runs, started by its own ID", t_smp_started },
    { "the AP trampoline's segments are all marked accessed", t_tramp_gdt_accessed },
    { "each AP: own GDT, TSS, GS, stacks; one IDT; PAT, MSRs", t_smp_ap_tables },
    { "an AP's own timer interrupts it, acknowledged",   t_smp_ap_timer },
    { "every CPU's TSC agrees, and one put out is set right", t_smp_tsc },
    { "an idle AP halts; a call wakes it once",           t_smp_parked },
    { "a changed kernel page is shot down on a busy AP",  t_tlb_kernel_busy },
    { "an idle AP is marked stale, not woken; flushes",   t_tlb_idle },
    { "a user space's change goes only where it runs",    t_tlb_user_targeted },
    { "shootdowns at each other or a spinner finish",     t_tlb_no_deadlock },
    { "an interrupts-off poll answers an AP's shootdown", t_tlb_poll_serves },
    { "a signature's arithmetic answers an AP's shootdown", t_tlb_modexp_serves },
    { "each CPU runs its idle thread, outside the count",  t_sched_idle_threads },
    { "a thread runs on the boot CPU, or the AP it is for", t_sched_placement },
    { "a wake from the boot CPU ends an idle AP's halt",   t_sched_wake_ipi },
    { "a sleep on an AP ends at its time",                 t_sched_ap_sleep },
    { "an AP's own timer ends a thread's slice",           t_sched_ap_slice },
    { "a thread raised takes a busy AP at once",           t_sched_outrank_ap },
    { "an affinity changed moves a running thread",        t_sched_affinity_moves },
    { "the storage lock keeps another CPU out, recursive", t_storage_lock },
    { "the log lock keeps another CPU out; the ring whole", t_log_lock },
    { "sysfile's files lock keeps another CPU out",      t_files_lock },
    { "the wall clock's CMOS reads take their lock",    t_cmos_lock },
    { "the input buffer's lock keeps another CPU out",   t_input_lock },
    { "the jobs' and processes' locks keep another CPU out", t_job_proc_locks },
    { "a process's output, drawing and fonts are locked", t_log_lock_draws },
    { "two last threads at once: one ends it, its code",  t_proc_last_exit },
    { "a ring-3 thread may run on any CPU",              t_user_threads_any_cpu },
    { "the APs reap exited threads, the boot CPU not needed", t_reap_on_ap },
    { "a thread woken before it is off its CPU stays there", t_sched_ready_on_cpu },
    { "an MSI vector reaches its handler, then nothing", t_msi_pool },
    { "syscall: EFER.SCE, STAR, LSTAR, FMASK; sysret's selectors", t_syscall_msrs },
    { "ring 3 starts at CPL 3, registers clear; SYS_EXIT", t_ring3_cpl },
    { "user code is R-X, the user stack RW- and no-execute", t_ring3_mappings },
    { "a syscall keeps every register but rax, rcx, r11", t_ring3_regs },
    { "SYS_CONSOLE_WRITE from ring 3; a bad buffer refused", t_ring3_write },
    { "a fault in ring 3 ends the process, not the kernel", t_ring3_fault },
    { "ring 3 cannot read kernel memory",                t_ring3_kernel_read },
    { "a user stack is no-execute",                      t_ring3_stack_nx },
    { "user code is not writable (W^X)",                 t_ring3_code_readonly },
    { "a user stack overflow faults on its guard page",  t_ring3_stack_guard },
    { "a kernel stack overflow is a double fault on IST 1", t_kernel_stack_overflow },
    { "a user copy: the check refuses, the copy recovers", t_usercopy },
    { "the tick preempts ring 3: two spinning processes end", t_ring3_preempted },
    { "a ring-3 process gives back everything it had",   t_ring3_releases },
};

const struct ktest_arch_case *ktest_arch_cases(unsigned *count) {
    *count = (unsigned)(sizeof cases / sizeof cases[0]);
    return cases;
}
