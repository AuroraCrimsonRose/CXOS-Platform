/* /CXLite/kernel/idt.c */
/* Aurora Tejeda */

#include "idt.h"

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

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

/* small self contained exception handler.
   Renders the panic either to the framebuffer (graphics mode) or straight to
   text VGA at 0xB8000 (text mode). We must handle graphics mode, or a fault
   while in graphics mode would write an invisible panic to 0xB8000 and just
   appear to hang. Kept self-contained (doesn't depend on the console) so it
   works even if the console is what crashed. */

#include "fb.h"

#define PANIC_ATTR 0x4F00   /* white on red (text mode) */

static int panic_col = 0;   /* framebuffer: current text column */
static int panic_row = 0;   /* framebuffer: current text row */
static int panic_pos = 0;   /* text mode: linear cell position */

static void panic_putc_at(char c) {
    if (fb_active()) {
        if (c == '\n') { panic_col = 0; panic_row++; return; }
        uint32_t white = fb_rgb(255, 255, 255);
        uint32_t red   = fb_rgb(170, 0, 0);
        fb_draw_char((uint32_t)(panic_col * FB_CHAR_W),
                     (uint32_t)(panic_row * (int)fb_font_height()),
                     c, white, red);
        panic_col++;
    } else {
        volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
        if (c == '\n') panic_pos = (panic_pos / 80 + 1) * 80;
        else           vga[panic_pos++] = (uint16_t)c | PANIC_ATTR;
    }
}

static void panic_puts(const char *s) {
    while (*s) panic_putc_at(*s++);
}

/* print a 32-bit value as 0x whatever  */
static void panic_puthex(uint32_t v) {
    const char *digits = "0123456789ABCDEF";
    panic_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        panic_putc_at(digits[(v >> i) & 0xF]);
}

/* push a CPU error code (others push our dummy 0) */
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

/* like idt_set_gate but DPL=3, so ring-3 code may invoke it (syscall gate). */
void idt_set_user_gate(int n, uint32_t handler) {
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = 0x08;
    idt[n].zero        = 0;
    idt[n].type_attr   = IDT_GATE_INT32_DPL3;
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

void isr_handler(struct registers *r) {
    panic_pos = 0;   /* text mode: top-left */
    panic_col = 0;   /* framebuffer: top-left */
    panic_row = 0;
    if (fb_active()) fb_clear(fb_rgb(170, 0, 0));   /* red screen */

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

    /* Page fault (exception 14): decode CR2 (faulting address) and the
       error-code bits, which say what went wrong. */
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
    }

    /* Dump EBP/ESP and walk the stack frames to show the call chain - the
       faulting EIP alone can be ambiguous, but the chain of return addresses
       tells us who called into the fault. Each frame: [saved EBP][return EIP]. */
    panic_puts("EBP:");  panic_puthex(r->ebp);
    panic_puts(" ESP:"); panic_puthex(r->esp);
    panic_puts("\n");
    panic_puts("Stack trace (return EIPs):\n");
    {
        uint32_t *fp = (uint32_t *)r->ebp;
        for (int i = 0; i < 6; i++) {
            /* bail if the frame pointer looks bogus (unmapped/low/odd) */
            if ((uint32_t)fp < 0x1000 || (uint32_t)fp > 0x08000000) break;
            uint32_t ret = fp[1];
            if (ret == 0) break;
            panic_puts("  ");
            panic_puthex(ret);
            panic_puts("\n");
            fp = (uint32_t *)fp[0];   /* previous frame */
        }
    }

    panic_puts("System halted.");
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

static void (*irq_routines[16])(struct registers *) = { 0 };

void irq_install_handler(int irq, void (*handler)(struct registers *)) {
    if (irq < 0 || irq >= 16) return;       /* ignore bad IRQ numbers */
    irq_routines[irq] = handler;
}

void irq_uninstall_handler(int irq) {
    if (irq < 0 || irq >= 16) return;
    irq_routines[irq] = 0;
}

void irq_handler(struct registers *r) {
    int irq = r->int_no - 32;
    if (irq < 0 || irq >= 16) {             /* defensive: only IRQs 0..15 */
        pic_send_eoi(r->int_no);            /* still ack so PIC isn't wedged */
        return;
    }
    void (*handler)(struct registers *) = irq_routines[irq];
    if (handler) handler(r);
    pic_send_eoi(r->int_no);
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
    idt_set_gate(8,(uint32_t)isr8);   idt_set_gate(9,(uint32_t)isr9);
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

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}