// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/boot64.c */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 kernel as roadmap step 5c leaves it: entered in long mode by
   stage 2 (boot/long.asm), it installs its own GDT, TSS and IDT, takes the
   loader's handover (boot/cxbi.c), replaces the loader's page tables with its
   own (paging.c), starts the page allocator (memman/pagedb.c), proves all of
   it with the bring-up self-tests below, says so on COM1, and halts. Its job
   moves to kmain once interrupts, the timer and the scheduler run here (step
   5d). Listed in docs/planning/TRANSITIONAL_CODE.md.

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
#include "pmm.h"

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

/* A page the tests map and unmap: the heap's region, which nothing uses on
   x86-64 until the heap runs here. */
#define SCRATCH_VA         KVA_HEAP

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

void boot64_main(uint64_t cxbi_phys) {
    cxbi_phys_addr = cxbi_phys;
    early_con_init();
    early_puts("\n");
    early_log("BOOT", EARLY_INFO, "CXK x86-64, roadmap step 5c: long mode, paging, page allocator");
    early_log_start("BOOT", EARLY_INFO);
    early_puts("CXBI at ");
    early_hex(cxbi_phys);
    early_puts("\n");

    gdt64_init();
    early_log("GDT", EARLY_OK, "GDT + TSS installed (double fault on IST 1)");
    idt64_init();
    early_log("IDT", EARLY_OK, "installed (exceptions 0-31)");

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
    early_log("BOOT", EARLY_INFO, "nothing more to run until step 5d; halted");
    for (;;) __asm__ volatile ("cli; hlt");
}
