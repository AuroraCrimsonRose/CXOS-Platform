// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/boot64.c */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 kernel as roadmap step 5d leaves it: entered in long mode by
   stage 2 (boot/long.asm), it installs its own GDT, TSS and IDT, takes the
   loader's handover (boot/cxbi.c), replaces the loader's page tables with its
   own (paging.c), starts the page allocator (memman/pagedb.c), and runs the
   first group of self-tests. Then kmain's order: the object caches, the heap
   and the guarded kernel stacks; onto a guarded stack as thread 0; the
   scheduler, the 1000 Hz tick, interrupts on, ACPI, and the local and I/O
   APIC; the second group of tests; the summary on COM1, and a halt. Its job
   moves to kmain once the drivers and ring 3 run here (steps 5e and 5f).
   Listed in docs/planning/TRANSITIONAL_CODE.md.

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

#define MSR_EFER    0xC0000080u
#define EFER_LME    (1ull << 8)
#define EFER_LMA    (1ull << 10)
#define EFER_NXE    (1ull << 11)

#define VEC_BREAKPOINT     3
#define VEC_INVALID_OPCODE 6
#define VEC_DOUBLE_FAULT   8
#define VEC_GP             13
#define VEC_PAGE_FAULT     14

/* the #PF error code */
#define PF_PRESENT         0x01u   /* a protection fault, not a missing page */
#define PF_WRITE           0x02u
#define PF_FETCH           0x10u   /* an instruction fetch: no-execute */

/* A selector one slot past the GDT's end, so loading it is a #GP whose error
   code is the selector itself. */
#define SEL_PAST_GDT       0x28

#define LOADER_PML4        0x3000u        /* boot/long.asm's PT_PML4 */
#define FOUR_GB            0x100000000ull
#define LAPIC_DEFAULT      0xFEE00000ull  /* the local APIC's registers, where firmware leaves them */

/* A page the tests map and unmap: the framebuffer's region, which nothing
   uses on x86-64 until the display runs here. */
#define SCRATCH_VA         KVA_FRAMEBUFFER

void boot64_main(uint64_t cxbi_phys);

extern uint8_t __kernel_end[];   /* the linker's, virtual */

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

static int t_top_2g(void) {
    uintptr_t rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    return (uintptr_t)&boot64_main >= KERNEL_VBASE && rsp >= KERNEL_VBASE;
}

/* The kernel's copy (boot/cxbi.c), taken while the loader's mapping of low
   memory was still there. */
static int t_cxbi(void) {
    if (cxbi_phys_addr == 0 || cxbi_phys_addr >= 0x40000000ull) return 0;
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

static uintptr_t main_stack_top;    /* thread 0's guarded stack (boot64_main) */

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

/* Thread 0 left the image's stack for a guarded one, as kmain's does: below
   it is a guard page that is not mapped and that kstack knows is thread 0's. */
static int t_main_stack_guarded(void) {
    uintptr_t rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    uintptr_t lo = main_stack_top - KSTACK_MAIN_SIZE;
    return main_stack_top != 0 && rsp < main_stack_top && rsp >= lo &&
           lo >= KSTACK_REGION_BASE && main_stack_top <= KSTACK_REGION_BASE + KSTACK_REGION_SIZE &&
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
volatile uint64_t boot64_regs_seen[5];
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
                      "movq %%rbx, boot64_regs_seen+0(%%rip)\n\t"
                      "movq %%r12, boot64_regs_seen+8(%%rip)\n\t"
                      "movq %%r13, boot64_regs_seen+16(%%rip)\n\t"
                      "movq %%r14, boot64_regs_seen+24(%%rip)\n\t"
                      "movq %%r15, boot64_regs_seen+32(%%rip)\n\t"
                      : : : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
                            "r10", "r11", "r12", "r13", "r14", "r15", "memory", "cc");
    settle();
    return clobber_ran && boot64_regs_seen[0] == 0x1111 && boot64_regs_seen[1] == 0x2222 &&
           boot64_regs_seen[2] == 0x3333 && boot64_regs_seen[3] == 0x4444 &&
           boot64_regs_seen[4] == 0x5555 && sched_active_count() == 1;
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

struct ktest64 {
    const char *name;
    int (*fn)(void);
};

/* Before the heap: the CPU's tables, paging, the page allocator. */
static const struct ktest64 early_tests[] = {
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
};

/* On thread 0's guarded stack, with the scheduler, the tick and the APICs. */
static const struct ktest64 late_tests[] = {
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
};

static unsigned run_tests(const struct ktest64 *t, unsigned n) {
    unsigned passed = 0;
    for (unsigned i = 0; i < n; i++) {
        int ok = t[i].fn();
        passed += ok ? 1u : 0u;
        early_log("KTEST", ok ? EARLY_OK : EARLY_FAIL, t[i].name);
    }
    return passed;
}

#define COUNT(a) (unsigned)(sizeof(a) / sizeof((a)[0]))

static unsigned early_passed;

static void boot64_late(void) __attribute__((noreturn));

void boot64_main(uint64_t cxbi_phys) {
    cxbi_phys_addr = cxbi_phys;
    early_con_init();
    early_puts("\n");
    early_log("BOOT", EARLY_INFO, "CXK x86-64, roadmap step 5d: interrupts, APIC, timer, scheduler");
    early_log_start("BOOT", EARLY_INFO);
    early_puts("CXBI at ");
    early_hex(cxbi_phys);
    early_puts("\n");

    arch_cpu_init();

    /* the copy is taken through the loader's window, so before paging_init */
    cxbi_init(cxbi_phys);
    paging_init();
    early_log("PAGING", EARLY_OK, "own tables: direct map, NX, kernel half made at boot");
    pmm_init();
    addr_space_init();
    paddr_t db;
    uint64_t db_len;
    pagedb_database(&db, &db_len);
    early_log_start("PMM", EARLY_OK);
    early_dec(pmm_free_count());
    early_puts(" of ");
    early_dec(pmm_total_pages());
    early_puts(" pages free; page database at ");
    early_hex(db);
    early_puts(", ");
    early_dec(db_len / 1024);
    early_puts(" KiB\n");

    /* Before anything is built on it: some of these check the boot stack. */
    early_passed = run_tests(early_tests, COUNT(early_tests));

    /* kmain's order from here (kernel/kmain.c). */
    ocache_sys_init();
    klog("OCACHE", SEV_OK, "object cache window reserved");
    heap_init();
    klog("HEAP", SEV_OK, "online (kmalloc/kfree)");
    kstack_init();
    klog("KSTACK", SEV_OK, "guarded kernel stacks ready");

    /* Leave the image's stack, as kmain does, so thread 0 overflowing is a
       fault on its guard page. Nothing on this stack is needed again. */
    main_stack_top = kstack_alloc(KSTACK_MAIN_SIZE, 0, 0);
    if (!main_stack_top) kpanic("no guarded stack for thread 0");
    arch_run_on_stack(main_stack_top, boot64_late);
}

static void boot64_late(void) {
    sched_init();
    /* Thread 0 gets an RSP0 stack of its own, as on x86-32 (kmain.c): ring 3
       comes in step 5e, and the RSP0 test is vacuous without one. */
    if (thread_alloc_kstack(0) != 0) kpanic("no RSP0 stack for thread 0");
    klog("SCHED", SEV_OK, "online");
    timer_init();
    arch_irq_enable();
    klog("TIMER", SEV_OK, "online (1000 Hz), interrupts enabled");
    timer_calibrate_tsc();
    klog_u32("TIMER", SEV_OK, "TSC calibrated, MHz: ", timer_tsc_mhz(), LOG_COLOR_VALUE, "");
    /* The MADT is the only description of the APICs, so ACPI first; with
       either missing the 8259 stays in charge, which works. */
    (void)acpi_init();
    (void)irq_controller_init();

    unsigned n = COUNT(early_tests) + COUNT(late_tests);
    unsigned passed = early_passed + run_tests(late_tests, COUNT(late_tests));

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
    early_log("BOOT", EARLY_INFO, "nothing more to run until step 5e; halted");
    for (;;) __asm__ volatile ("cli; hlt");
}
