/* /CXLite/kernel/idt.h */
/* Aurora Tejeda */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;
    uint16_t offset_high;
} __attribute__((packed));


struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct registers {
    uint32_t ds;                                     
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; 
    uint32_t int_no, err_code;                       
    uint32_t eip, cs, eflags, useresp, ss;           
} __attribute__((packed));

void idt_init(void);

/* install a handler reachable from ring 3 (DPL=3) - for the syscall gate. */
void idt_set_user_gate(int n, uint32_t handler);

void irq_install_handler(int irq, void (*handler)(struct registers *));
void irq_uninstall_handler(int irq);

#define IDT_GATE_INT32 0x8E
#define IDT_GATE_INT32_DPL3 0xEE   /* present, DPL=3, 32-bit interrupt gate */

#endif