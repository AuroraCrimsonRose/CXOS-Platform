// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/gdt.c */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 GDT and TSS (gdt.h). In long mode a code or data descriptor is
   almost all ignored bits: what is left is present, DPL, and L for code. The
   TSS no longer holds a task's registers; it holds the stacks the CPU switches
   to - RSP0 for an entry from ring 3, and the seven IST stacks an IDT gate can
   name. */

#include <stddef.h>
#include "gdt.h"
#include "cpu/percpu.h"

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];            /* rsp[0]: the stack for an entry from ring 3 */
    uint64_t reserved1;
    uint64_t ist[7];            /* IST1..IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

_Static_assert(sizeof(struct tss64) == 104, "the 64-bit TSS is 104 bytes");

struct gdt_ptr64 {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define GDT64_COUNT 7

/* Flat descriptors: limit 0xFFFFF, G set, base 0. Code is L=1 D=0. */
#define DESC_KCODE  0x00AF9A000000FFFFull   /* present, ring 0, code, L */
#define DESC_KDATA  0x00CF92000000FFFFull   /* present, ring 0, data, writable */
#define DESC_UDATA  0x00CFF2000000FFFFull   /* present, ring 3, data, writable */
#define DESC_UCODE  0x00AFFA000000FFFFull   /* present, ring 3, code, L */
#define TSS_TYPE_AVAILABLE  0x89u           /* present, 64-bit TSS, not busy */

#define IST_STACK_SIZE  GDT64_IST_STACK_SIZE
#define RSP0_STACK_SIZE 8192

/* One GDT and TSS per CPU (1.9d): ltr marks a TSS busy, so two CPUs cannot
   load one, and each needs its own IST and RSP0 stacks besides. The boot
   CPU's stacks are static, the others' come with them (smp.c). Which is
   this CPU's is cpu_this()->index. */
struct gdt64_cpu {
    uint64_t     gdt[GDT64_COUNT] __attribute__((aligned(16)));
    struct tss64 tss __attribute__((aligned(16)));
    uintptr_t    rsp0_default;
    uintptr_t    ist_lo[IST_STACKS];
};

static struct gdt64_cpu gdt_cpu[CPU_MAX];
static uint8_t ist_stack[IST_STACKS][IST_STACK_SIZE] __attribute__((aligned(16)));
/* RSP0 while the running thread has no kernel stack of its own (arch/cpu.h's
   arch_kernel_stack_default), as x86-32's tss_kernel_stack. */
static uint8_t rsp0_stack[RSP0_STACK_SIZE] __attribute__((aligned(16)));

static struct gdt64_cpu *mine(void) { return &gdt_cpu[cpu_this()->index]; }

static void set_tss_descriptor(struct gdt64_cpu *g) {
    uint64_t base  = (uint64_t)(uintptr_t)&g->tss;
    uint64_t limit = sizeof(g->tss) - 1;
    unsigned slot  = GDT64_TSS / 8;

    g->gdt[slot] = (limit & 0xFFFF)
                 | ((base & 0xFFFFFF) << 16)
                 | ((uint64_t)TSS_TYPE_AVAILABLE << 40)
                 | (((limit >> 16) & 0xF) << 48)
                 | (((base >> 24) & 0xFF) << 56);
    g->gdt[slot + 1] = base >> 32;          /* the upper half of the base */
}

static void load(struct gdt64_cpu *g) {
    struct gdt_ptr64 gdtr = { sizeof(g->gdt) - 1, (uint64_t)(uintptr_t)g->gdt };
    __asm__ volatile ("lgdt %0" : : "m"(gdtr) : "memory");

    /* CS can only be reloaded by a far transfer: push the selector and the
       return address and lretq to the next line. */
    __asm__ volatile (
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movl %[ds], %%eax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        : : [cs] "i"(GDT64_KCODE), [ds] "i"(GDT64_KDATA) : "rax", "memory");

    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)GDT64_TSS) : "memory");
}

int gdt64_init_cpu(unsigned index, const uintptr_t ist_lo[IST_STACKS], uintptr_t rsp0_top) {
    if (index >= CPU_MAX) return -1;
    struct gdt64_cpu *g = &gdt_cpu[index];
    g->gdt[0] = 0;
    g->gdt[GDT64_KCODE / 8] = DESC_KCODE;
    g->gdt[GDT64_KDATA / 8] = DESC_KDATA;
    g->gdt[GDT64_UDATA / 8] = DESC_UDATA;
    g->gdt[GDT64_UCODE / 8] = DESC_UCODE;

    for (unsigned i = 0; i < IST_STACKS; i++) {
        g->ist_lo[i] = ist_lo[i];
        g->tss.ist[i] = (uint64_t)(ist_lo[i] + IST_STACK_SIZE);
    }
    g->rsp0_default = rsp0_top;
    g->tss.rsp[0] = rsp0_top;
    g->tss.iomap_base = sizeof(g->tss);     /* no I/O bitmap: ring 3 gets no ports */
    set_tss_descriptor(g);
    load(g);
    return 0;
}

void gdt64_init(void) {
    uintptr_t lo[IST_STACKS];
    for (unsigned i = 0; i < IST_STACKS; i++) lo[i] = (uintptr_t)ist_stack[i];
    (void)gdt64_init_cpu(0, lo, (uintptr_t)(rsp0_stack + RSP0_STACK_SIZE));
}

uintptr_t gdt64_base(void) { return (uintptr_t)mine()->gdt; }

uint64_t gdt64_descriptor(unsigned n) { return n < GDT64_COUNT ? mine()->gdt[n] : 0; }

uint64_t gdt64_tss_descriptor(void) { return mine()->gdt[GDT64_TSS / 8]; }

uintptr_t gdt64_tss_base(void) { return (uintptr_t)&mine()->tss; }

void gdt64_ist_stack(unsigned ist, uintptr_t *lo, uintptr_t *hi) {
    if (ist < 1 || ist > IST_STACKS) { *lo = *hi = 0; return; }
    struct gdt64_cpu *g = mine();
    *lo = g->ist_lo[ist - 1];
    *hi = (uintptr_t)g->tss.ist[ist - 1];
}

void gdt64_set_rsp0(uintptr_t top) { mine()->tss.rsp[0] = (uint64_t)top; }

uintptr_t gdt64_rsp0(void) { return (uintptr_t)mine()->tss.rsp[0]; }

uintptr_t gdt64_default_rsp0(void) { return mine()->rsp0_default; }
