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
#include "arch/ktest_arch.h"
#include "arch/usermode.h"

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
} expect;

static int probe(struct registers *f) {
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

/* All 256 kernel entries exist from boot, kernel-only; the user half is
   empty. */
static int t_kernel_half(void) {
    const uint64_t *pml4 = paging_temp_map(addr_space_kernel());
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
    const uint64_t *pml4 = paging_temp_map(addr_space_kernel());
    unsigned leaves = 0;
    for (unsigned i = PML4_INDEX(DIRECT_MAP_BASE);
         i <= PML4_INDEX(DIRECT_MAP_BASE + DIRECT_MAP_SIZE - 1); i++) {
        if (!(pml4[i] & PTE64_PRESENT)) continue;
        const uint64_t *pdpt = paging_temp_map(pml4[i] & PTE64_ADDR);
        for (unsigned j = 0; j < PT64_ENTRIES; j++) {
            if (!(pdpt[j] & PTE64_PRESENT)) continue;
            if (pdpt[j] & PTE64_LARGE) return 0;        /* none are made */
            const uint64_t *pd = paging_temp_map(pdpt[j] & PTE64_ADDR);
            for (unsigned k = 0; k < PT64_ENTRIES; k++) {
                if (!(pd[k] & PTE64_PRESENT)) continue;
                if (pd[k] & PTE64_LARGE) {
                    if (!direct_map_leaf_ok(pd[k])) return 0;
                    leaves++;
                    continue;
                }
                const uint64_t *pt = paging_temp_map(pd[k] & PTE64_ADDR);
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
    const uint64_t *mine = paging_temp_map(root);
    const uint64_t *kern = paging_temp_map(addr_space_kernel());
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
    const uint64_t *t = paging_temp_map(addr_space_current());
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        uint64_t e = t[(virt >> shift) & 511u];
        if (!(e & PTE64_PRESENT) || (e & PTE64_LARGE)) return 0;
        t = paging_temp_map(e & PTE64_ADDR);
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

static int t_ticks(void) { return ticks_advance(5); }

/* irq_controller_init took over: the 8259 is masked outright, the I/O APIC
   routes the tick, and its EOI goes to the local APIC - an EOI sent to the
   wrong controller stops the tick after the first one. */
static int t_io_apic(void) {
    return lapic_active() && apic_active() && pic_get_masks() == 0xFFFF &&
           ticks_advance(5);
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
   only because the tick switches between them. */
static volatile uint32_t pre_a, pre_b;
static void pre_thread_a(void) { for (uint32_t i = 0; i < 3000000u; i++) pre_a = i; }
static void pre_thread_b(void) { for (uint32_t i = 0; i < 3000000u; i++) pre_b = i; }

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
    *(volatile uint64_t *)(uintptr_t)U_SP = u->arg;
    u->code_pte  = entry_of(U_CODE);
    u->stack_pte = entry_of(U_STACK);
    u->set_up = 1;
    u->rc = enter_usermode(U_CODE, U_SP, thread_current_usave());
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
    int id = thread_create_process("ring3", urun_thread);
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
    return (uint32_t)(u.rc & 0xFFFF) == len && ((u.rc >> 16) & 0xFF) == 0xFF &&
           syscall_trapped_count() - t0 == 3 && syscall_dispatch_count() - d0 == 3 &&
           s1.entered - s0.entered == 3 && s1.exited - s0.exited == 2 &&
           s1.last_num == SYS_CONSOLE_WRITE;
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
    ok = ok && user_copy_in(buf, U_STACK, sizeof buf) == E_FAULT &&
               user_copy_out(U_CODE, buf, sizeof buf) == E_FAULT &&
               usercopy_faults_recovered() == r0;
    ok = ok && user_copy_bytes(user_ptr(U_CODE), buf, sizeof buf) == 1 &&
               user_copy_bytes(buf, user_ptr(U_STACK), sizeof buf) == 1 &&
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
    volatile uint32_t *word = paging_temp_map(sh);
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

/* ---- arch/ktest_arch.h ---- */

/* The double fault's IST stack (t_idt checks the gate names it): the wiring
   ktest_run asks every architecture about, by taking one. */
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
    { "the user half's last page is never mapped",     t_user_top_page },
    { "kmalloc: small from the caches, large from the list", t_heap },
    { "thread 0 runs on a guarded kernel stack",        t_main_stack_guarded },
    { "the PIT ticks with interrupts on",               t_ticks },
    { "the I/O APIC routes the tick; the 8259 is masked", t_io_apic },
    { "100 ticks are 100 ms of the ACPI PM timer",      t_tick_rate },
    { "a 50 ms sleep is 50 to 100 ticks",               t_sleep },
    { "two threads that yield both run to the end",     t_coop },
    { "a switch keeps rbx and r12-r15",                 t_switch_keeps_registers },
    { "a new thread starts with interrupts on",         t_new_thread_irqs_on },
    { "the tick preempts threads that never yield",     t_preempt },
    { "RSP0 follows the running thread",                t_rsp0_follows_thread },
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
