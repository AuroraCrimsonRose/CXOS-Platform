/* /CXK/kernel/cpu/gdt.c */
/* Aurora Tejeda / CATX Systems LLC */
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

#define GDT_COUNT 6
static struct gdt_entry gdt[GDT_COUNT];
static struct gdt_ptr   gdtr;
static struct tss_entry tss;

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