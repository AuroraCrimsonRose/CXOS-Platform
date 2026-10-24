// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/gdt.c */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 GDT and TSS (gdt.h). In long mode a code or data descriptor is
   almost all ignored bits: what is left is present, DPL, and L for code. The
   TSS no longer holds a task's registers; it holds the stacks the CPU switches
   to - RSP0 for an entry from ring 3, and the seven IST stacks an IDT gate can
   name. */

#include "gdt.h"

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];            /* the stack for an entry from ring 3 (step 5e) */
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

#define GDT64_COUNT 5

/* Flat descriptors: limit 0xFFFFF, G set, base 0. Code is L=1 D=0. */
#define DESC_KCODE  0x00AF9A000000FFFFull   /* present, ring 0, code, L */
#define DESC_KDATA  0x00CF92000000FFFFull   /* present, ring 0, data, writable */
#define TSS_TYPE_AVAILABLE  0x89u           /* present, 64-bit TSS, not busy */

#define DF_STACK_SIZE 8192

static uint64_t gdt[GDT64_COUNT] __attribute__((aligned(16)));
static struct tss64 tss __attribute__((aligned(16)));
static uint8_t df_stack[DF_STACK_SIZE] __attribute__((aligned(16)));

static void set_tss_descriptor(void) {
    uint64_t base  = (uint64_t)(uintptr_t)&tss;
    uint64_t limit = sizeof(tss) - 1;

    gdt[3] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | ((uint64_t)TSS_TYPE_AVAILABLE << 40)
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[4] = base >> 32;                    /* the upper half of the base */
}

void gdt64_init(void) {
    gdt[0] = 0;
    gdt[1] = DESC_KCODE;
    gdt[2] = DESC_KDATA;

    tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(uintptr_t)(df_stack + DF_STACK_SIZE);
    tss.iomap_base = sizeof(tss);           /* no I/O bitmap: ring 3 gets no ports */
    set_tss_descriptor();

    struct gdt_ptr64 gdtr = { sizeof(gdt) - 1, (uint64_t)(uintptr_t)gdt };
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

uintptr_t gdt64_base(void) { return (uintptr_t)gdt; }

uint64_t gdt64_tss_descriptor(void) { return gdt[3]; }

void gdt64_df_stack(uintptr_t *lo, uintptr_t *hi) {
    *lo = (uintptr_t)df_stack;
    *hi = (uintptr_t)(df_stack + DF_STACK_SIZE);
}
