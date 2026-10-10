// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/gdt.c */
/* Aurora Tejeda / CATX Systems */
/* Kernel GDT + TSS setup for ring 0/3 + privilege transitions. */

#include "gdt.h"

/* a GDT segment descriptor (8 bytes) */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;   /* high nibble = flags, low nibble = limit[19:16] */
    uint8_t  base_high;
} __attribute__((packed));

/* the GDTR operand for lgdt */
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* the 32-bit Task State Segment. We only use ss0/esp0 (the ring-0 stack the CPU
   loads on a ring3->ring0 transition). Everything else is zero. */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;     /* ring-0 stack pointer (the important field) */
    uint32_t ss0;      /* ring-0 stack segment */
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

#define GDT_COUNT 7
static struct gdt_entry gdt[GDT_COUNT];
static struct gdt_ptr   gdtr;
static struct tss_entry tss;

/* The double-fault task. A double fault is delivered through a task gate to
   this TSS, so the CPU loads a complete fresh context from it - above all its
   own stack - instead of pushing onto whatever stack faulted. On 32-bit x86
   that is the only way to guarantee a stack for the handler; x86-64 replaced
   it with the IST. See double_fault_task in idt.c. */
#define DF_STACK_SIZE 8192
static struct tss_entry df_tss;
static uint8_t df_stack[DF_STACK_SIZE] __attribute__((aligned(16)));
extern void double_fault_task(void);

/* defined in gdt_flush.asm: load gdtr, reload segment regs, far-jump CS */
extern void gdt_flush(uint32_t gdtr_addr);
/* load the task register with the TSS selector */
extern void tss_flush(uint32_t tss_selector);

static void set_gate(int i, uint32_t base, uint32_t limit,
                     uint8_t access, uint8_t flags) {
    gdt[i].limit_low        = (uint16_t)(limit & 0xFFFF);
    gdt[i].base_low         = (uint16_t)(base & 0xFFFF);
    gdt[i].base_mid         = (uint8_t)((base >> 16) & 0xFF);
    gdt[i].access           = access;
    gdt[i].flags_limit_high = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    gdt[i].base_high        = (uint8_t)((base >> 24) & 0xFF);
}

/* Dedicated ring-0 stack for the TSS (esp0). This MUST be separate from the
   kernel's main stack: when a syscall/interrupt arrives from ring 3, the CPU
   switches to this stack. If it shared the main kernel stack, the syscall's
   stack usage would clobber frames saved there by enter_usermode, corrupting
   the return path. A BSS array is mapped + supervisor-only + collision-free. */
#define TSS_STACK_SIZE 8192
static uint8_t tss_kernel_stack[TSS_STACK_SIZE] __attribute__((aligned(16)));

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}

void tss_faulted_state(struct tss_fault *out) {
    out->eip    = tss.eip;
    out->esp    = tss.esp;
    out->ebp    = tss.ebp;
    out->cs     = tss.cs;
    out->eflags = tss.eflags;
}

static void df_tss_init(void) {
    for (unsigned i = 0; i < sizeof(df_tss); i++) ((uint8_t *)&df_tss)[i] = 0;
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    /* The kernel's own page directory. Every address space shares the kernel
       half, so the handler, its stack and the display are mapped in it
       whichever space was live when the fault happened. */
    df_tss.cr3    = cr3;
    df_tss.eip    = (uint32_t)double_fault_task;
    df_tss.eflags = 0x2;                 /* reserved bit only: interrupts OFF */
    df_tss.esp    = (uint32_t)(df_stack + DF_STACK_SIZE);
    df_tss.ebp    = 0;                   /* ends the handler's own stack trace */
    df_tss.cs     = KERNEL_CODE_SEL;
    df_tss.ss     = df_tss.ds = df_tss.es = df_tss.fs = df_tss.gs = KERNEL_DATA_SEL;
    df_tss.ss0    = KERNEL_DATA_SEL;
    df_tss.esp0   = df_tss.esp;
    df_tss.iomap_base = sizeof(struct tss_entry);
}

void gdt_init(void) {
    /* 0: null */
    set_gate(0, 0, 0, 0, 0);
    /* 1: ring-0 code  (0x08)  access 0x9A, flags 0xC0 (G=1, DB=1) */
    set_gate(1, 0, 0xFFFFF, 0x9A, 0xC0);
    /* 2: ring-0 data  (0x10)  access 0x92 */
    set_gate(2, 0, 0xFFFFF, 0x92, 0xC0);
    /* 3: ring-3 code  (0x18)  access 0xFA (DPL=3) */
    set_gate(3, 0, 0xFFFFF, 0xFA, 0xC0);
    /* 4: ring-3 data  (0x20)  access 0xF2 (DPL=3) */
    set_gate(4, 0, 0xFFFFF, 0xF2, 0xC0);

    /* 5: TSS descriptor (0x28). access 0x89 = present, DPL0, type=32-bit TSS
       (available). limit = sizeof(tss)-1, granularity byte = 0 (byte units). */
    uint32_t tss_base  = (uint32_t)&tss;
    uint32_t tss_limit = sizeof(struct tss_entry) - 1;
    set_gate(5, tss_base, tss_limit, 0x89, 0x00);

    /* 6: the double-fault task's TSS (0x30), same shape. Never loaded into TR:
       only the task gate at IDT vector 8 switches to it. */
    df_tss_init();
    set_gate(6, (uint32_t)&df_tss, tss_limit, 0x89, 0x00);

    /* initialize the TSS: ring-0 stack = kernel data segment + the kernel stack.
       iomap_base past the limit = no I/O bitmap. */
    for (unsigned i = 0; i < sizeof(tss); i++) ((uint8_t *)&tss)[i] = 0;
    tss.ss0  = KERNEL_DATA_SEL;          /* 0x10 */
    tss.esp0 = (uint32_t)(tss_kernel_stack + TSS_STACK_SIZE);  /* dedicated stack top */
    tss.iomap_base = sizeof(struct tss_entry);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint32_t)&gdt[0];

    gdt_flush((uint32_t)&gdtr);
    tss_flush(TSS_SEL);
}