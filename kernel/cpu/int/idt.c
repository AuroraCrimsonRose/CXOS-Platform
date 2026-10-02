// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/int/idt.c */
/* Aurora Tejeda / CATX Systems */
/*
 * v5 IDT: exception + IRQ handling with a self-contained panic dump.
 * Differences from the v4 port (deliberate for the current v5 stage):
 *  - PANIC renders to whichever display is live: the framebuffer (red screen,
 *    white 8x16 text) when a graphics mode is active, else VGA text via its
 *    HIGHER-HALF address (0xC00B8000) - v5 runs higher-half with the identity
 *    map gone, so physical 0xB8000 is NOT mapped.
 *  - NO SCHEDULER HOOK YET (sched not ported); the timer-IRQ preempt call is
 *    omitted and clearly marked for re-adding with the process model.
 *  - user-fault hook kept (no dependency) for the process model to register.
 * Self-contained (no console dependency) so it works even if the console crashed.
 */

#include "idt.h"
#include "apic.h"
#include "syslog.h"
#include "format.h"
#include "sched.h"
#include "speaker.h"
#include "fb.h"
#include "gdt.h"
#include "kstack.h"
#include "paging.h"

void pic_remap(void);
void pic_send_eoi(uint32_t int_no);

extern void isr0();  extern void isr1();  extern void isr2();  extern void isr3();
extern void isr4();  extern void isr5();  extern void isr6();  extern void isr7();
extern void isr8();  extern void isr9();  extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14(); extern void isr15();
extern void isr16(); extern void isr17(); extern void isr18(); extern void isr19();
extern void isr20(); extern void isr21(); extern void isr22(); extern void isr23();
extern void isr24(); extern void isr25(); extern void isr26(); extern void isr27();
extern void isr28(); extern void isr29(); extern void isr30(); extern void isr31();
extern void irq0();  extern void irq1();  extern void irq2();  extern void irq3();
extern void irq4();  extern void irq5();  extern void irq6();  extern void irq7();
extern void irq8();  extern void irq9();  extern void irq10(); extern void irq11();
extern void irq12(); extern void irq13(); extern void irq14(); extern void irq15();
extern void irq16(); extern void irq17(); extern void irq18(); extern void irq19();
extern void irq20(); extern void irq21(); extern void irq22(); extern void irq23();

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

/* higher-half VGA text buffer (physical 0xB8000 mapped at 0xC0000000+) */
#define VGA_PANIC ((volatile uint16_t *)(0xC0000000 + 0xB8000))
#define PANIC_ATTR 0x4F00   /* white on red */

static int panic_pos = 0;

/* Write one panic character to whichever display is live. On the framebuffer we
   render white-on-red 8x16 glyphs (the screen is cleared to red when the panic
   begins, in isr_handler); otherwise we poke white-on-red VGA cells. panic_pos
   is a linear cell index in both cases, wrapped at the live width. */
static void panic_putc_at(char c) {
    if (fb_active()) {
        uint32_t cols = fb_width() / 8;
        if (cols == 0) cols = 1;
        if (c == '\n') { panic_pos = ((panic_pos / (int)cols) + 1) * (int)cols; return; }
        uint32_t col = (uint32_t)panic_pos % cols;
        uint32_t row = (uint32_t)panic_pos / cols;
        if ((row + 1) * 16 <= fb_height())
            fb_draw_char(col * 8, row * 16, c, fb_rgb(255, 255, 255), fb_rgb(0xAA, 0, 0));
        panic_pos++;
        return;
    }
    if (c == '\n') { panic_pos = (panic_pos / 80 + 1) * 80; return; }
    VGA_PANIC[panic_pos++] = (uint16_t)c | PANIC_ATTR;
}
static void panic_puts(const char *s) { while (*s) panic_putc_at(*s++); }
static void panic_puthex(uint32_t v) {
    char buf[9];
    panic_puts("0x");
    fmt_hex(buf, v, 8, 1);   /* 8-digit, uppercase, zero-padded */
    panic_puts(buf);
}
static int has_error_code(uint32_t n) {
    return (n == 8) || (n >= 10 && n <= 14) || (n == 17);
}

static const char *exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "BOUND Range", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coproc Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 FPU Error", "Alignment Check", "Machine Check", "SIMD FP",
    "Reserved","Reserved","Reserved","Reserved",
    "Reserved","Reserved","Reserved","Reserved",
    "Reserved","Reserved","Reserved","Reserved"
};

static void idt_set_gate(int n, uint32_t handler) {
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = 0x08;
    idt[n].zero        = 0;
    idt[n].type_attr   = IDT_GATE_INT32;
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}
/* A task gate: the CPU switches to the task in TSS `tss_sel`, with that task's
   own stack and registers. Only the double fault uses one (see below). */
static void idt_set_task_gate(int n, uint16_t tss_sel) {
    idt[n].offset_low  = 0;
    idt[n].selector    = tss_sel;
    idt[n].zero        = 0;
    idt[n].type_attr   = IDT_GATE_TASK;
    idt[n].offset_high = 0;
}
void idt_set_user_gate(int n, uint32_t handler) {
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = 0x08;
    idt[n].zero        = 0;
    idt[n].type_attr   = IDT_GATE_INT32_DPL3;
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

static void panic_tail(uint32_t ebp);

/* If `addr` is in the guard below a live kernel stack, say whose. A large
   frame can jump into the guard while the exception frame itself still fits,
   so an overflow arrives as a plain page fault as often as a double fault. */
static void panic_name_overflow(uint32_t addr) {
    int owner = kstack_guard_owner(addr);
    if (owner < 0) return;
    char num[12];
    fmt_u32(num, (uint32_t)owner);
    panic_puts("Kernel stack overflow: thread ");
    panic_puts(num);
    const char *name = thread_name(owner);
    if (name) { panic_puts(" ("); panic_puts(name); panic_puts(")"); }
    panic_puts("\n");
}

static void (*user_fault_hook)(struct registers *r) = 0;
void set_user_fault_hook(void (*hook)(struct registers *)) { user_fault_hook = hook; }

void isr_handler(struct registers *r) {
    speaker_panic_tone();
    if ((r->cs & 3) == 3 && user_fault_hook) {
        user_fault_hook(r);
    }

    panic_pos = 0;
    if (fb_active()) fb_clear(fb_rgb(0xAA, 0, 0));   /* red screen for the panic */
    panic_puts("*** KERNEL PANIC ***\n");
    panic_puts("Exception: ");
    panic_puthex(r->int_no);
    panic_puts("  ");
    if (r->int_no < 32) panic_puts(exception_names[r->int_no]);
    panic_puts("\n");

    panic_puts("EIP: ");
    panic_puthex(r->eip);
    panic_puts("   ERR: ");
    if (has_error_code(r->int_no)) panic_puthex(r->err_code);
    else                          panic_puts("(none)");
    panic_puts("\n");

    panic_puts("EAX:");  panic_puthex(r->eax);
    panic_puts(" EBX:"); panic_puthex(r->ebx);
    panic_puts(" ECX:"); panic_puthex(r->ecx);
    panic_puts(" EDX:"); panic_puthex(r->edx);
    panic_puts("\n");

    if (r->int_no == 14) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        panic_puts("Faulting addr (CR2): ");
        panic_puthex(cr2);
        panic_puts("\n");
        panic_puts("Cause: ");
        panic_puts((r->err_code & 0x1) ? "protection" : "not-present");
        panic_puts((r->err_code & 0x2) ? ", write" : ", read");
        panic_puts((r->err_code & 0x4) ? ", user" : ", kernel");
        panic_puts("\n");
        panic_name_overflow(cr2);
    }

    panic_puts("EBP:");  panic_puthex(r->ebp);
    panic_puts(" ESP:"); panic_puthex(r->esp);
    panic_puts("\n");
    panic_tail(r->ebp);
}

/* The common end of every panic: a stack trace from `ebp`, the recent log,
   the crash log to disk, and a halt. */
static void panic_tail(uint32_t ebp) {
    panic_puts("Stack trace (return EIPs):\n");
    {
        uint32_t *fp = (uint32_t *)ebp;
        for (int i = 0; i < 6; i++) {
            if ((uint32_t)fp < 0x1000) break;   /* higher-half frames OK */
            /* A frame pointer from a thread that ran off its stack can point
               into the guard. Reading it would fault again, inside the panic;
               stop the trace instead. */
            if (!paging_get_phys((uint32_t)fp) || !paging_get_phys((uint32_t)fp + 4)) break;
            uint32_t ret = fp[1];
            if (ret == 0) break;
            panic_puts("  ");
            panic_puthex(ret);
            panic_puts("\n");
            fp = (uint32_t *)fp[0];
        }
    }

    /* recent system-log entries (last 8) - what was happening before the fault */
    {
        uint32_t n = slog_count();
        uint32_t start = (n > 8u) ? (n - 8u) : 0u;
        panic_puts("Recent log:\n");
        for (uint32_t i = start; i < n; i++) {
            const struct slog_entry *e = slog_get(i);
            if (!e) break;
            panic_puts("  ");
            panic_puts(e->system);
            if (e->subsystem[0]) { panic_puts("/"); panic_puts(e->subsystem); }
            panic_puts(": ");
            panic_puts(e->event);
            panic_puts("\n");
        }
    }

    if (slog_flush_crash() == 0) panic_puts("Log saved to /System/crash.log\n");

    panic_puts("System halted.");
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

/* ---- double fault ---------------------------------------------------------
 * Reached through a TASK GATE (gdt.c), not an interrupt gate, so it runs on a
 * stack of its own. That is the whole point: the usual way to double fault is
 * a kernel stack overflow - the page fault on the guard cannot push its frame
 * onto the stack that just ran out, so the CPU escalates. Delivered on that
 * same stack, the double fault would fail too, and a third fault resets the
 * machine with nothing on screen. As a task it gets a fresh stack, and the
 * state of the code that faulted is saved in the main TSS for us to report.
 *
 * Never returns: the faulting context is unrecoverable by definition. */
void double_fault_task(void) {
    struct tss_fault f;
    tss_faulted_state(&f);
    uint32_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    speaker_panic_tone();
    panic_pos = 0;
    if (fb_active()) fb_clear(fb_rgb(0xAA, 0, 0));
    panic_puts("*** KERNEL PANIC ***\n");
    panic_puts("Exception: 0x00000008  Double Fault (on its own stack)\n");

    /* The overflow case, named: the page-fault address, or failing that the
       stack pointer, in the guard below a live kernel stack. */
    panic_name_overflow(kstack_guard_owner(cr2) >= 0 ? cr2 : f.esp);

    panic_puts("EIP: ");  panic_puthex(f.eip);
    panic_puts("   CR2: "); panic_puthex(cr2);
    panic_puts("\n");
    panic_puts("EBP:");  panic_puthex(f.ebp);
    panic_puts(" ESP:"); panic_puthex(f.esp);
    panic_puts("\n");
    panic_tail(f.ebp);
}

static void (*irq_routines[16])(struct registers *) = { 0 };

void irq_install_handler(int irq, void (*handler)(struct registers *)) {
    if (irq < 0 || irq >= 16) return;
    irq_routines[irq] = handler;
}
void irq_uninstall_handler(int irq) {
    if (irq < 0 || irq >= 16) return;
    irq_routines[irq] = 0;
}

/* Acknowledge at whichever controller actually delivered this. Sending an EOI
   to the 8259 while the I/O APIC is in charge leaves the local APIC's
   in-service bit set, and nothing of equal or lower priority is ever delivered
   again - the machine goes quiet rather than crashing. */
static inline void irq_eoi(uint32_t int_no) {
    if (apic_active()) lapic_eoi();
    else               pic_send_eoi(int_no);
}

/* MSI handlers, indexed by vector - MSI_VECTOR_BASE. Separate from
   irq_routines because an MSI has no IRQ number: it is a bare vector. */
static void (*msi_routines[MSI_VECTOR_COUNT])(struct registers *) = { 0 };

int irq_alloc_msi_vector(void (*handler)(struct registers *)) {
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

void irq_handler(struct registers *r) {
    int irq = r->int_no - 32;

    /* An MSI arrives as a vector with no line behind it. It still needs an EOI
       to the local APIC - that is what delivered it. */
    if (r->int_no >= MSI_VECTOR_BASE &&
        r->int_no < MSI_VECTOR_BASE + MSI_VECTOR_COUNT) {
        void (*h)(struct registers *) = msi_routines[r->int_no - MSI_VECTOR_BASE];
        if (h) h(r);
        irq_eoi(r->int_no);
        return;
    }

    if (irq < 0 || irq >= 16) {
        irq_eoi(r->int_no);
        return;
    }
    void (*handler)(struct registers *) = irq_routines[irq];
    if (handler) handler(r);
    irq_eoi(r->int_no);

    /* deferred preemption: the timer's sched_tick() may have set need_resched.
       Perform the actual context switch HERE, after the EOI, so the PIC is
       ready to deliver the next timer IRQ to the thread we switch to (the
       EOI-ordering fix). Safe to call always - no-op unless a switch is due. */
    sched_preempt_point();
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0; idt[i].selector = 0; idt[i].zero = 0;
        idt[i].type_attr = 0;  idt[i].offset_high = 0;
    }

    pic_remap();

    idt_set_gate(0,(uint32_t)isr0);   idt_set_gate(1,(uint32_t)isr1);
    idt_set_gate(2,(uint32_t)isr2);   idt_set_gate(3,(uint32_t)isr3);
    idt_set_gate(4,(uint32_t)isr4);   idt_set_gate(5,(uint32_t)isr5);
    idt_set_gate(6,(uint32_t)isr6);   idt_set_gate(7,(uint32_t)isr7);
    idt_set_task_gate(8, DF_TSS_SEL); idt_set_gate(9,(uint32_t)isr9);
    idt_set_gate(10,(uint32_t)isr10); idt_set_gate(11,(uint32_t)isr11);
    idt_set_gate(12,(uint32_t)isr12); idt_set_gate(13,(uint32_t)isr13);
    idt_set_gate(14,(uint32_t)isr14); idt_set_gate(15,(uint32_t)isr15);
    idt_set_gate(16,(uint32_t)isr16); idt_set_gate(17,(uint32_t)isr17);
    idt_set_gate(18,(uint32_t)isr18); idt_set_gate(19,(uint32_t)isr19);
    idt_set_gate(20,(uint32_t)isr20); idt_set_gate(21,(uint32_t)isr21);
    idt_set_gate(22,(uint32_t)isr22); idt_set_gate(23,(uint32_t)isr23);
    idt_set_gate(24,(uint32_t)isr24); idt_set_gate(25,(uint32_t)isr25);
    idt_set_gate(26,(uint32_t)isr26); idt_set_gate(27,(uint32_t)isr27);
    idt_set_gate(28,(uint32_t)isr28); idt_set_gate(29,(uint32_t)isr29);
    idt_set_gate(30,(uint32_t)isr30); idt_set_gate(31,(uint32_t)isr31);

    idt_set_gate(32,(uint32_t)irq0);  idt_set_gate(33,(uint32_t)irq1);
    idt_set_gate(34,(uint32_t)irq2);  idt_set_gate(35,(uint32_t)irq3);
    idt_set_gate(36,(uint32_t)irq4);  idt_set_gate(37,(uint32_t)irq5);
    idt_set_gate(38,(uint32_t)irq6);  idt_set_gate(39,(uint32_t)irq7);
    idt_set_gate(40,(uint32_t)irq8);  idt_set_gate(41,(uint32_t)irq9);
    idt_set_gate(42,(uint32_t)irq10); idt_set_gate(43,(uint32_t)irq11);
    idt_set_gate(44,(uint32_t)irq12); idt_set_gate(45,(uint32_t)irq13);
    idt_set_gate(46,(uint32_t)irq14); idt_set_gate(47,(uint32_t)irq15);

    /* MSI vectors. Above the legacy IRQ range so nothing collides with a pin. */
    idt_set_gate(48,(uint32_t)irq16); idt_set_gate(49,(uint32_t)irq17);
    idt_set_gate(50,(uint32_t)irq18); idt_set_gate(51,(uint32_t)irq19);
    idt_set_gate(52,(uint32_t)irq20); idt_set_gate(53,(uint32_t)irq21);
    idt_set_gate(54,(uint32_t)irq22); idt_set_gate(55,(uint32_t)irq23);

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}