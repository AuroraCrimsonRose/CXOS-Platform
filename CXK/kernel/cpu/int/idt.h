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

/* register a handler for faults that occur in ring 3 (so the process model can
   kill the offending process instead of panicking). */
void set_user_fault_hook(void (*hook)(struct registers *));

void irq_install_handler(int irq, void (*handler)(struct registers *));

/* ---- MSI vectors --------------------------------------------------------
 * An MSI has no IRQ number - the device writes a vector straight to the local
 * APIC - so these are allocated from a pool above the legacy IRQ range rather
 * than installed against a line. */
#define MSI_VECTOR_BASE  48
#define MSI_VECTOR_COUNT 8

/* Claim a vector and attach a handler. Returns the vector to program into the
   device's MSI capability, or -1 if none are free. */
int  irq_alloc_msi_vector(void (*handler)(struct registers *));
void irq_free_msi_vector(int vector);
void irq_uninstall_handler(int irq);

#define IDT_GATE_INT32 0x8E
#define IDT_GATE_INT32_DPL3 0xEE   /* present, DPL=3, 32-bit interrupt gate */
#define IDT_GATE_TASK       0x85   /* present, DPL=0, task gate */

/* the double-fault handler: entered as its own task through a task gate */
void double_fault_task(void);

#endif