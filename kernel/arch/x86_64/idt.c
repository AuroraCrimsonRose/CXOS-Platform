// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/idt.c */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 IDT (idt.h) and arch/irq.h. Every gate is an interrupt gate, so
   the CPU clears IF on entry. An exception no self-test asked for, and no user
   fault hook takes, is a panic: the banner cxk verify-boot looks for, then the
   vector, the error code and the registers, on COM1, and the CPU halts. A
   device interrupt runs its handler, is acknowledged at whichever controller
   delivered it, and then lets the scheduler switch if a tick asked for one -
   x86-32's idt.c, the same order for the same reasons. */

#include "idt.h"
#include "gdt.h"
#include "early_con.h"
#include "panic.h"
#include "sched.h"
#include "arch/x86/apic.h"
#include "platform/pc/pic.h"
#include "usermode.h"          /* usercopy_start, usercopy_end, usercopy_trampoline */
#include "paging_defs.h"       /* USER_VLIMIT */

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
#define VEC_DEBUG         1
#define VEC_NMI           2
#define VEC_DOUBLE_FAULT  8
#define VEC_PAGE_FAULT    14
#define VEC_MACHINE_CHECK 18

#define STUB_COUNT        (MSI_VECTOR_BASE + MSI_VECTOR_COUNT)

extern const uint64_t isr64_stubs[STUB_COUNT];  /* isr.asm */
extern void isr64_spurious(void);

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

/* The gates that must not trust RSP (gdt.h): the double fault, and what can
   arrive in the syscall entry's first instructions, before it leaves the
   user's stack. */
static unsigned ist_of(unsigned vector) {
    switch (vector) {
    case VEC_DOUBLE_FAULT:  return IST_DOUBLE_FAULT;
    case VEC_NMI:           return IST_NMI;
    case VEC_DEBUG:         return IST_DEBUG;
    case VEC_MACHINE_CHECK: return IST_MACHINE_CHECK;
    default:                return 0;
    }
}

void idt64_init(void) {
    /* The 8259 off the exception vectors before anything can unmask a line:
       IRQ n arrives at 32 + n, every line as masked as the BIOS left it. */
    pic_remap();

    for (unsigned v = 0; v < STUB_COUNT; v++)
        set_gate(v, isr64_stubs[v], ist_of(v));
    set_gate(SPURIOUS_VECTOR64, (uint64_t)(uintptr_t)isr64_spurious, 0);

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

/* ---- arch/irq.h ---- */

static irq_handler_t irq_routines[16];
/* MSI handlers, by vector - MSI_VECTOR_BASE: an MSI is a bare vector with no
   line behind it. */
static irq_handler_t msi_routines[MSI_VECTOR_COUNT];
static void (*user_fault_hook)(struct registers *);

void irq_install_handler(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16) irq_routines[irq] = handler;
}

void irq_uninstall_handler(int irq) {
    if (irq >= 0 && irq < 16) irq_routines[irq] = 0;
}

int irq_alloc_msi_vector(irq_handler_t handler) {
    for (int i = 0; i < MSI_VECTOR_COUNT; i++) {
        if (!msi_routines[i]) {
            msi_routines[i] = handler;
            return MSI_VECTOR_BASE + i;
        }
    }
    return -1;
}

void irq_free_msi_vector(int vector) {
    int i = vector - MSI_VECTOR_BASE;
    if (i >= 0 && i < MSI_VECTOR_COUNT) msi_routines[i] = 0;
}

void set_user_fault_hook(void (*hook)(struct registers *)) { user_fault_hook = hook; }

uint32_t  irq_frame_vector(const struct registers *r) { return (uint32_t)r->vector; }
uintptr_t irq_frame_ip(const struct registers *r)     { return (uintptr_t)r->rip; }
uint32_t  irq_frame_error(const struct registers *r)  { return (uint32_t)r->error; }

uintptr_t irq_fault_address(void) {
    uintptr_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

/* Acknowledge at whichever controller delivered it. An EOI sent to the 8259
   while the I/O APIC is in charge leaves the local APIC's in-service bit set,
   and nothing of equal or lower priority is delivered again (x86-32 idt.c). */
static inline void irq_eoi(uint64_t vector) {
    if (apic_active()) lapic_eoi();
    else               pic_send_eoi((uint32_t)vector);
}

static void irq_dispatch(struct registers *r) {
    if (r->vector >= MSI_VECTOR_BASE) {
        /* An MSI still takes an EOI, at the local APIC that delivered it. */
        irq_handler_t h = msi_routines[r->vector - MSI_VECTOR_BASE];
        if (h) h(r);
        irq_eoi(r->vector);
        return;
    }
    irq_handler_t h = irq_routines[r->vector - IRQ_VECTOR_BASE];
    if (h) h(r);
    irq_eoi(r->vector);

    /* The deferred preemption sched_tick may have asked for, after the EOI so
       the controller delivers the next tick to the thread switched to. */
    sched_preempt_point();
}

/* A page fault inside user_copy_bytes (usermode.asm) is the one kernel fault
   that is not a bug: a user buffer that stopped being mapped, or was never
   what its check thought. The copy returns 1 from its landing pad instead,
   and user_copy_out/in make that E_FAULT. The same four conditions as
   x86-32's idt.c, for the same reason: recovery is a property of where the
   fault happened, with no flag that could be left armed. */
static int recover_user_copy_fault(struct registers *f) {
    if (f->vector != VEC_PAGE_FAULT) return 0;
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    if (cr2 >= USER_VLIMIT)  return 0;     /* a user address */
    if (f->error & 0x4)      return 0;     /* taken in ring 0: the kernel did it */
    if (f->error & 0x10)     return 0;     /* data, not an instruction fetch */
    if (f->rip <  (uint64_t)(uintptr_t)usercopy_start) return 0;
    if (f->rip >= (uint64_t)(uintptr_t)usercopy_end)   return 0;
    usercopy_note_recovered_fault();
    f->rip = (uint64_t)(uintptr_t)usercopy_trampoline;
    return 1;
}

void idt64_dispatch(struct registers *f);
void idt64_dispatch(struct registers *f) {
    if (f->vector >= IRQ_VECTOR_BASE) {
        irq_dispatch(f);
        return;
    }

    /* Before the probe and the panic: a recovered fault leaves no trace. */
    if (recover_user_copy_fault(f)) return;

    trap_probe_fn p = probe;
    if (p && p(f)) return;

    /* A fault in ring 3 ends the process rather than the kernel (step 5e). */
    if ((f->cs & 3) && user_fault_hook) {
        user_fault_hook(f);
        return;
    }

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

/* panic.h's kpanic, until the panic screen and the crash log run on x86-64:
   the banner verify-boot looks for, the reason, and a halt. */
void kpanic(const char *why) {
    __asm__ volatile ("cli");
    early_puts("\n*** KERNEL PANIC ***\n");
    early_puts(why);
    early_puts("\nSystem halted.\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
