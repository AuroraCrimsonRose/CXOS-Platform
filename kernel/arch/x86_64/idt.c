// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/idt.c */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 IDT (idt.h). Every exception gate is an interrupt gate, so the
   CPU clears IF on entry. An exception no self-test asked for is a panic: the
   banner cxk verify-boot looks for, then the vector, the error code and the
   registers, on COM1, and the CPU halts. */

#include "idt.h"
#include "gdt.h"
#include "early_con.h"

struct idt_gate64 {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;               /* bits 0-2; 0 = stay on the current stack */
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct idt_gate64) == 16, "a 64-bit IDT gate is 16 bytes");

struct idt_ptr64 {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define IDT64_COUNT       256
#define GATE_INTERRUPT    0x8Eu     /* present, ring 0, 64-bit interrupt gate */
#define VEC_DOUBLE_FAULT  8

extern const uint64_t isr64_stubs[32];      /* isr.asm */

static struct idt_gate64 idt[IDT64_COUNT] __attribute__((aligned(16)));
static volatile trap_probe_fn probe;

static const char *const exception_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
    "invalid opcode", "device not available", "double fault",
    "coprocessor segment overrun", "invalid TSS", "segment not present",
    "stack fault", "general protection", "page fault", "reserved",
    "x87 floating point", "alignment check", "machine check",
    "SIMD floating point", "virtualization", "control protection",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
    "hypervisor injection", "VMM communication", "security", "reserved",
};

static void set_gate(unsigned vector, uint64_t handler, unsigned ist) {
    struct idt_gate64 *g = &idt[vector];
    g->offset_low  = (uint16_t)handler;
    g->selector    = GDT64_KCODE;
    g->ist         = (uint8_t)(ist & 7);
    g->type_attr   = GATE_INTERRUPT;
    g->offset_mid  = (uint16_t)(handler >> 16);
    g->offset_high = (uint32_t)(handler >> 32);
    g->reserved    = 0;
}

void idt64_init(void) {
    for (unsigned v = 0; v < 32; v++)
        set_gate(v, isr64_stubs[v], v == VEC_DOUBLE_FAULT ? IST_DOUBLE_FAULT : 0);

    struct idt_ptr64 idtr = { sizeof(idt) - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(idtr) : "memory");
}

void idt64_set_probe(trap_probe_fn p) { probe = p; }

uintptr_t idt64_base(void) { return (uintptr_t)idt; }

unsigned idt64_gate_ist(unsigned vector) {
    return vector < IDT64_COUNT ? idt[vector].ist & 7u : 0;
}

static void reg(const char *name, uint64_t v) {
    early_puts(name);
    early_hex(v);
}

void idt64_dispatch(struct trap_frame64 *f);
void idt64_dispatch(struct trap_frame64 *f) {
    trap_probe_fn p = probe;
    if (p && p(f)) return;

    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    early_puts("\n*** KERNEL PANIC ***\n");
    early_puts("CPU exception ");
    early_dec(f->vector);
    early_puts(" (");
    early_puts(f->vector < 32 ? exception_names[f->vector] : "unknown");
    early_puts(")");
    reg(", error ", f->error);
    reg("\n RIP ", f->rip);   reg("  CS ", f->cs);   reg("  RFLAGS ", f->rflags);
    reg("\n RSP ", f->rsp);   reg("  SS ", f->ss);   reg("  CR2 ", cr2);
    reg("\n RAX ", f->rax);   reg("  RBX ", f->rbx); reg("  RCX ", f->rcx);
    reg("\n RDX ", f->rdx);   reg("  RSI ", f->rsi); reg("  RDI ", f->rdi);
    reg("\n RBP ", f->rbp);   reg("  frame ", (uint64_t)(uintptr_t)f);
    early_puts("\nSystem halted.\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
