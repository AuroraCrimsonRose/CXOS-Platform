// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/arch/x86_64/idt.c */
/* Aurora Tejeda / CATX Systems */
/* The x86-64 IDT (idt.h) and arch/irq.h. Every gate is an interrupt gate, so
   the CPU clears IF on entry. An exception no self-test asked for, and no user
   fault hook takes, is a panic: the banner cxk verify-boot looks for, then the
   vector, the error code and the registers, on COM1, the screen and the crash
   log (cxk rig crashlog), and the CPU halts. A
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
#include "paging_defs.h"       /* USER_VLIMIT, KERNEL_VBASE */
#include "fb.h"                /* the panic screen */
#include "random.h"            /* random_irq_sample */
#include "platform/pc/speaker.h"
#include "diag/crashlog.h"     /* the panic's record on the CRASH partition */
#include "sync/lockorder.h"    /* lockorder_stop */
#include "cpu/percpu.h"        /* cpu_this, cpu_boot */
#include "smp.h"               /* smp_ap_interrupt */
#include "arch/smp.h"          /* smp_serve */
#include "timer.h"             /* timer_hints_take (1.9e2) */
#include "disk.h"              /* storage_break (1.9e3) */
#include "console.h"           /* log_break (1.9e3) */
#include "diag/panicrep.h"
#include "diag/panicscreen.h"
#include "diag/ksym.h"
#include "arch/paging.h"       /* paging_get_phys: the trace reads only what is mapped */
#include "cxk_abi.h"           /* CXK_ABI_VERSION */
#include "cxk_version.h"       /* CXK_VERSION_STR, generated */

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

#define STUB_COUNT        (SMP_IPI_VECTOR + 1)
_Static_assert(LAPIC_TIMER_VECTOR == MSI_VECTOR_BASE + MSI_VECTOR_COUNT,
               "the local APIC timer's stub follows the MSI pool's (isr.asm)");
_Static_assert(SMP_IPI_VECTOR == LAPIC_TIMER_VECTOR + 1,
               "the wake IPI's stub follows the timer's (isr.asm)");

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

    idt64_load();
}

/* One table for every CPU: a gate names code and an IST slot, and each CPU
   reads the slot from its own TSS (gdt.c), so nothing in it is per CPU. */
void idt64_load(void) {
    struct idt_ptr64 idtr = { sizeof(idt) - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(idtr) : "memory");
}

void idt64_set_probe(trap_probe_fn p) { probe = p; }

uintptr_t idt64_base(void) { return (uintptr_t)idt; }

unsigned idt64_gate_ist(unsigned vector) {
    return vector < IDT64_COUNT ? idt[vector].ist & 7u : 0;
}

/* ---- the panic's output: x86-32's panic_putc_at, in its order ----
 * The crash log's buffer first, then COM1, then the screen: a panic is the
 * moment the display is most likely to be the broken thing, so the bytes are
 * out of the machine before a framebuffer pointer is touched. White on red,
 * on the framebuffer when one is live, otherwise in VGA text. Without the
 * screen and the crash log, a panic on a machine with no serial port (the
 * rig) looks like a hang and leaves nothing behind. */
#define VGA_PANIC  ((volatile uint16_t *)(KERNEL_VBASE + 0xB8000))
#define PANIC_ATTR 0x4F00

static int panic_pos;

/* The report's text: on every output but a framebuffer, which gets the
   friendly screen instead (diag/panicscreen.c). */
static void panic_putc(char c) {
    crashlog_putc(c);
    early_putc(c);

    if (fb_active()) return;
    if (c == '\n') { panic_pos = (panic_pos / 80 + 1) * 80; return; }
    if (panic_pos < 80 * 25) VGA_PANIC[panic_pos++] = (uint16_t)(uint8_t)c | PANIC_ATTR;
}

static void panic_puts(const char *s) { while (*s) panic_putc(*s++); }

static void panic_hex(uint64_t v) {
    panic_puts("0x");
    for (int i = 60; i >= 0; i -= 4) panic_putc("0123456789ABCDEF"[(v >> i) & 0xF]);
}

static void panic_dec(uint64_t v) {
    char d[20];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) panic_putc(d[--n]);
}

static void reg(const char *name, uint64_t v) {
    panic_puts(name);
    panic_hex(v);
}

/* The CPU's per-CPU pointer, or 0 before it is set: a panic that early has
   no per-CPU data to read, and reading %gs:0 then would fault. */
static struct cpu *panic_cpu(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101u));   /* IA32_GS_BASE */
    return (struct cpu *)(uintptr_t)(((uint64_t)hi << 32) | lo);
}

__attribute__((noreturn)) static void panic_halt(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

/* One report. Index + 1 of the CPU making it; 0 before there is one. */
static uint32_t panic_owner;
static int panic_nested;

/* A panic while this CPU was already reporting one: the report's own code
   faulted, the likeliest cause being what caused the first. Say so on COM1,
   try once to save what the first one had got as far as, and stop. */
__attribute__((noreturn)) static void panic_again(const char *why, uint64_t rip) {
    if (panic_nested++ == 0) {
        early_puts("\n*** panic inside the panic: ");
        early_puts(why ? why : "(no reason given)");
        early_puts(" at ");
        for (int i = 60; i >= 0; i -= 4) early_putc("0123456789ABCDEF"[(rip >> i) & 0xF]);
        early_puts(" ***\n");
        (void)crashlog_panic_save();
    }
    panic_halt();
}

/* The first CPU to panic reports; a later one stops where it is, so that two
   do not share the report's static buffers. */
static void panic_begin(const char *why, uint64_t rip) {
    struct cpu *c = panic_cpu();
    uint32_t me = (c ? c->index : 0) + 1, none = 0;
    if (!__atomic_compare_exchange_n(&panic_owner, &none, me, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (none == me) panic_again(why, rip);
        panic_halt();
    }
#if CXK_LOCK_CHECK
    lockorder_stop();   /* the crash path takes locks in whatever state it finds */
#endif
    /* A CPU that held either when another panicked would keep the crash
       record and the screen from ever being written: the crash path goes
       through them unlocked (sync/rspin.h). */
    storage_break();
    log_break();
    /* Before the screen: a display server still drawing on another CPU would
       draw over the report. */
    if (c) smp_panic_stop();
    speaker_panic_tone();
    panic_pos = 0;
    fb_panic_takeover();   /* from the display server, if it holds the screen */
    if (fb_active()) fb_clear(fb_rgb(0xAA, 0, 0));
    early_puts("\n");   /* the banner starts a line of its own on COM1 */
}

/* ---- the report ---- */

static struct panic_report report;

/* A frame must be aligned and mapped, whole, in the kernel's half: a wild
   frame pointer ends the trace here, and does not fault the panic. */
static int frame_readable(uint64_t fp) {
    if ((fp & 7) || fp < 0xFFFF800000000000ull || fp > 0xFFFFFFFFFFFFFFE0ull) return 0;
    return paging_get_phys((uintptr_t)fp) != 0 && paging_get_phys((uintptr_t)fp + 15) != 0;
}

unsigned panic_stack_walk(uint64_t rbp, uint64_t *pcs, unsigned max) {
    extern const char __text_start[], __text_end[];
    unsigned n = 0;
    while (n < max && frame_readable(rbp)) {
        const uint64_t *frame = (const uint64_t *)(uintptr_t)rbp;
        uint64_t next = frame[0], pc = frame[1];
        if (pc < (uint64_t)(uintptr_t)__text_start || pc >= (uint64_t)(uintptr_t)__text_end) break;
        pcs[n++] = pc;
        if (next <= rbp) break;            /* a stack grows one way */
        rbp = next;
    }
    return n;
}

static void report_add_frame(uint64_t pc) {
    if (report.nframes >= PANIC_FRAMES_MAX) return;
    struct panic_frame *f = &report.frames[report.nframes++];
    f->addr = pc;
    f->name = ksym_lookup((uintptr_t)pc, &f->off);
}

static void report_who(void) {
    report.version = CXK_VERSION_STR;
    report.abi = CXK_ABI_VERSION;
    struct cpu *c = panic_cpu();
    report.cpu = c ? c->index : 0;
    report.tid = c ? thread_current_id() : -1;
    report.pid = c ? thread_current_pid() : -1;
    report.thread = c ? thread_name(report.tid) : 0;
}

/* Everything that goes in the crash record and on COM1. */
static void report_print(void) {
    panic_puts("Stop code: ");
    panic_puts(report.stop_name);
    panic_puts(" (CX-");
    for (int i = 12; i >= 0; i -= 4) panic_putc("0123456789ABCDEF"[(report.stop_code >> i) & 0xF]);
    panic_puts("), crash number ");
    for (int i = 28; i >= 0; i -= 4) panic_putc("0123456789ABCDEF"[(report.crash_id >> i) & 0xF]);
    panic_puts("\nCXK ");
    panic_puts(report.version);
    panic_puts(", ABI ");
    panic_dec(report.abi);
    panic_puts(", CPU ");
    panic_dec(report.cpu);
    panic_puts(", thread ");
    panic_dec((uint64_t)(uint32_t)report.tid);
    if (report.thread) { panic_puts(" ("); panic_puts(report.thread); panic_puts(")"); }
    panic_puts(", pid ");
    panic_dec((uint64_t)(uint32_t)report.pid);
    panic_puts("\nStack trace:\n");
    for (unsigned i = 0; i < report.nframes; i++) {
        panic_puts(" ");
        panic_hex(report.frames[i].addr);
        if (report.frames[i].name) {
            panic_puts(" ");
            panic_puts(report.frames[i].name);
            panic_puts("+0x");
            for (int d = 12; d >= 0; d -= 4)    /* as the screen and the code spell it */
                panic_putc("0123456789ABCDEF"[(report.frames[i].off >> d) & 0xF]);
        }
        panic_puts("\n");
    }
}

/* The system log's tail goes in the record (crashlog_panic_save); on
   screen, whether it was saved. Halts. */
__attribute__((noreturn)) static void panic_end(void) {
    report_print();
    panic_screen_show(&report);
    panic_puts("\n");   /* the record's system-log tail starts a line */
    int saved = crashlog_panic_save();
    if (saved == 0) {
        panic_puts("Crash log saved (cxk rig crashlog)");
        panic_screen_status("Crash log saved.");
    } else {
        panic_puts("Crash log not saved, code ");
        panic_hex((uint64_t)(uint32_t)saved);
        panic_screen_status("Crash log not saved.");
    }
    panic_puts("\nSystem halted.\n");
    panic_screen_status("System halted.");
    panic_halt();
}

/* ---- arch/irq.h ---- */

static irq_handler_t irq_routines[16];
/* MSI handlers, by vector - MSI_VECTOR_BASE: an MSI is a bare vector with no
   line behind it. */
static irq_handler_t msi_routines[MSI_VECTOR_COUNT];
static void (*user_fault_hook)(struct registers *);
static void (*user_irq_return_hook)(void);
static int  (*demand_fault_hook)(uintptr_t addr, int write, int exec);

void irq_install_handler(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16) irq_routines[irq] = handler;
}

void irq_uninstall_handler(int irq) {
    if (irq >= 0 && irq < 16) irq_routines[irq] = 0;
}

/* A slot is taken by a compare-and-swap: since 2.2 a process asks for one
   (SYS_DEVICE_IRQ) from any CPU, and two asking at once must not both be
   given it. */
int irq_alloc_msi_vector(irq_handler_t handler) {
    for (int i = 0; i < MSI_VECTOR_COUNT; i++) {
        irq_handler_t none = 0;
        if (__atomic_compare_exchange_n(&msi_routines[i], &none, handler, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return MSI_VECTOR_BASE + i;
    }
    return -1;
}

void irq_free_msi_vector(int vector) {
    int i = vector - MSI_VECTOR_BASE;
    if (i >= 0 && i < MSI_VECTOR_COUNT) __atomic_store_n(&msi_routines[i], 0, __ATOMIC_RELEASE);
}

void set_user_fault_hook(void (*hook)(struct registers *)) { user_fault_hook = hook; }
void set_user_irq_return_hook(void (*hook)(void)) { user_irq_return_hook = hook; }
void set_demand_fault_hook(int (*hook)(uintptr_t, int, int)) { demand_fault_hook = hook; }

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
    /* An AP has nothing routed to it: what it takes is its IPI (a
       shootdown, a call, a thread handed to it) and its own timer (1.9e2:
       its slice), each counted and acknowledged at its own local APIC, then
       a preemption point. No tick and no sample for the generator: the
       boot CPU alone keeps time and feeds it. */
    if (cpu_this() != &cpu_boot) {
        smp_ap_interrupt((uint32_t)r->vector);
        lapic_eoi();
        if (r->vector == LAPIC_TIMER_VECTOR) sched_cpu_tick();
        sched_preempt_point();
        return;
    }
    /* 1.9e1: an AP's shootdown, aimed at the boot CPU; 1.9e2: a thread
       handed to it, or a deadline an AP set (timer.c). Its own branch: past
       the MSI pool, which the branch below would index beyond its end. */
    if (r->vector == SMP_IPI_VECTOR) {
        smp_serve();
        timer_hints_take();
        lapic_eoi();
        sched_preempt_point();
        return;
    }
    /* The cycle counter at every interrupt, stored for the generator to hash
       later: a store, never a hash, here (random.h). */
    random_irq_sample((uint32_t)r->vector);
    if (r->vector == LAPIC_TIMER_VECTOR) {
        /* The tick's, when the local APIC's timer gives it (timer.c): its
           EOI is the local APIC's whoever routes the IRQs, and it is a
           preemption point as IRQ 0 is. */
        lapic_timer_interrupt(r);
        lapic_eoi();
        sched_preempt_point();
        return;
    }
    if (r->vector >= MSI_VECTOR_BASE) {
        /* An MSI still takes an EOI, at the local APIC that delivered it. */
        irq_handler_t h = __atomic_load_n(&msi_routines[r->vector - MSI_VECTOR_BASE],
                                          __ATOMIC_ACQUIRE);
        if (h) h(r);
        irq_eoi(r->vector);
        /* A handler may have woken a thread that should run now: a driver's
           interrupt thread (2.2), at real-time priority. */
        sched_preempt_point();
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
   and user_copy_out/in make that E_BAD_ADDRESS. The same four conditions as
   x86-32's idt.c, for the same reason: recovery is a property of where the
   fault happened, with no flag that could be left armed. */
/* A page that was not present, at a user address, touched from ring 3 or from
   inside user_copy_bytes: its first touch, if a region holds it (memman/
   vmregion.h), and the faulting instruction runs again. Anywhere else in the
   kernel a missing user page is a bug, as before: the checks in front of a
   dereference fault the pages in (cpu/usercopy.c). */
static int demand_fault(struct registers *f) {
    if (f->vector != VEC_PAGE_FAULT || !demand_fault_hook) return 0;
    if (f->error & 0x1) return 0;          /* present: a protection fault */
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    if (cr2 >= USER_VLIMIT) return 0;
    if (!(f->cs & 3) &&
        (f->rip < (uint64_t)(uintptr_t)usercopy_start ||
         f->rip >= (uint64_t)(uintptr_t)usercopy_end)) return 0;
    return demand_fault_hook((uintptr_t)cr2, (f->error & 0x2) != 0, (f->error & 0x10) != 0);
}

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
        /* A preemption out of ring 3 has come back by now; a thread killed
           meanwhile leaves here rather than running on in ring 3. */
        if ((f->cs & 3) && user_irq_return_hook) user_irq_return_hook();
        return;
    }

    /* A first touch is not a fault at all. */
    if (demand_fault(f)) return;

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

    const char *name = f->vector < 32 ? exception_names[f->vector] : "unknown";
    panic_begin(name, f->rip);
    panic_puts("*** KERNEL PANIC ***\n");
    panic_puts("CPU exception ");
    panic_dec(f->vector);
    panic_puts(" (");
    panic_puts(name);
    panic_puts(")");
    reg(", error ", f->error);
    reg("\n RIP ", f->rip);   reg("  CS ", f->cs);   reg("  RFLAGS ", f->rflags);
    reg("\n RSP ", f->rsp);   reg("  SS ", f->ss);   reg("  CR2 ", cr2);
    reg("\n RAX ", f->rax);   reg("  RBX ", f->rbx); reg("  RCX ", f->rcx);
    reg("\n RDX ", f->rdx);   reg("  RSI ", f->rsi); reg("  RDI ", f->rdi);
    reg("\n RBP ", f->rbp);   reg("  frame ", (uint64_t)(uintptr_t)f);
    panic_puts("\n");

    report.stop_name = panic_stop_name((uint32_t)f->vector);
    report.stop_code = (uint32_t)f->vector;
    report.why = name;
    report.has_regs = 1;
    report.rip = f->rip;     report.rsp = f->rsp;     report.rbp = f->rbp;
    report.cr2 = cr2;        report.error = f->error;
    report.rax = f->rax;     report.rbx = f->rbx;     report.rcx = f->rcx;
    report.rdx = f->rdx;     report.rsi = f->rsi;     report.rdi = f->rdi;
    report_who();
    report_add_frame(f->rip);
    {
        static uint64_t pcs[PANIC_FRAMES_MAX];
        unsigned n = panic_stack_walk(f->rbp, pcs, PANIC_FRAMES_MAX - 1);
        for (unsigned i = 0; i < n; i++) report_add_frame(pcs[i]);
    }
    report.crash_id = panic_crash_id(&report);
    panic_end();
}

/* panic.h's kpanic: the banner verify-boot looks for, the reason and the
   caller, on every output, then the crash log and a halt. */
void kpanic(const char *why) {
    __asm__ volatile ("cli");
    uint64_t from = (uint64_t)(uintptr_t)__builtin_return_address(0);
    panic_begin(why, from);
    panic_puts("*** KERNEL PANIC ***\n");
    panic_puts(why ? why : "(no reason given)");
    reg("\nCalled from RIP: ", from);
    panic_puts("\n");

    report.stop_name = "KERNEL_CHECK_FAILED";
    report.stop_code = PANIC_STOP_CHECK;
    report.why = why ? why : "(no reason given)";
    report.rip = from;
    report_who();
    {
        /* The walk's first frame is kpanic's own return address: `from`. */
        static uint64_t pcs[PANIC_FRAMES_MAX];
        unsigned n = panic_stack_walk((uint64_t)(uintptr_t)__builtin_frame_address(0), pcs, PANIC_FRAMES_MAX);
        for (unsigned i = 0; i < n; i++) report_add_frame(pcs[i]);
    }
    report.crash_id = panic_crash_id(&report);
    panic_end();
}
