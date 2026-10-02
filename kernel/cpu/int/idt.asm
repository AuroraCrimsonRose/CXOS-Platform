; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /CXLite/kernel/idt.asm
; Aurora Tejeda

bits 32

extern isr_handler          ; C function: void isr_handler(struct registers*)
extern irq_handler          ; C function: void irq_handler(struct registers*)

; Exceptions that DON'T push an error code
; layout is identical for every vector.
%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push dword 0            
    push dword %1           
    jmp isr_common
%endmacro

; Exceptions that DO push their own error code
%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push dword %1           ; vector number (err code already on stack)
    jmp isr_common
%endmacro

%macro IRQ 2
global irq%2
irq%2:
    cli
    push dword 0            ; dummy error code
    push dword %1          ; vector number (32..47)
    jmp irq_common
%endmacro

; CPU exceptions 0-31
ISR_NOERR 0     ; divide error
ISR_NOERR 1     ; debug
ISR_NOERR 2     ; NMI
ISR_NOERR 3     ; breakpoint
ISR_NOERR 4     ; overflow
ISR_NOERR 5     ; bound range
ISR_NOERR 6     ; invalid opcode
ISR_NOERR 7     ; device not available
ISR_ERR   8     ; double fault (has error code)
ISR_NOERR 9     ; coprocessor segment overrun
ISR_ERR   10    ; invalid TSS
ISR_ERR   11    ; segment not present
ISR_ERR   12    ; stack-segment fault
ISR_ERR   13    ; general protection fault
ISR_ERR   14    ; page fault
ISR_NOERR 15    ; reserved
ISR_NOERR 16    ; x87 FPU error
ISR_ERR   17    ; alignment check
ISR_NOERR 18    ; machine check
ISR_NOERR 19    ; SIMD FP
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

IRQ 32, 0       ; timer (PIT)
IRQ 33, 1       ; keyboard
IRQ 34, 2
IRQ 35, 3
IRQ 36, 4
IRQ 37, 5
IRQ 38, 6
IRQ 39, 7
IRQ 40, 8
IRQ 41, 9
IRQ 42, 10
IRQ 43, 11
IRQ 44, 12
IRQ 45, 13
IRQ 46, 14
IRQ 47, 15

; ---- MSI vectors ----
; MSI is delivered as a plain interrupt vector with no IRQ line behind it, so
; these need gates of their own. They reuse the IRQ stub because the entry and
; exit sequence is identical - only the dispatch in irq_handler differs.
IRQ 48, 16
IRQ 49, 17
IRQ 50, 18
IRQ 51, 19
IRQ 52, 20
IRQ 53, 21
IRQ 54, 22
IRQ 55, 23

isr_common:
    pushad                  ; eax,ecx,edx,ebx,esp,ebp,esi,edi
    mov ax, ds
    push eax                ; save data segment

    mov ax, 0x10            ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                ; pass pointer to struct registers
    call isr_handler
    add esp, 4

    pop eax                 ; restore data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popad
    add esp, 8              ; drop vector number + error code
    sti
    iret

irq_common:
    pushad
    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler
    add esp, 4

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popad
    add esp, 8
    sti
    iret