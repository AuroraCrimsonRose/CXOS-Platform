; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /kernel/arch/x86_64/isr.asm  -  the CPU exception entry stubs
; Aurora Tejeda / CATX Systems
;
; One stub per exception vector 0-31. Each makes the frame uniform - a zero
; where the CPU pushes no error code, then the vector - saves the general
; registers and calls idt64_dispatch(struct trap_frame64 *). Whatever the
; handler leaves in the frame is what iretq resumes, RIP and RSP included.
; The layout is struct trap_frame64 in idt.h; keep the two in step.
;
; In 64-bit mode the CPU aligns RSP to 16 before it pushes its frame, so the
; 5 + 2 + 15 = 22 quadwords below leave the call 16-byte aligned, as the
; SysV ABI wants. The device interrupts arrive in roadmap step 5d.

bits 64
default rel

section .text
extern idt64_dispatch

%macro ISR_NOERR 1
isr%1:
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push %1                         ; the CPU pushed the error code
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    cld
    call idt64_dispatch
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                     ; the vector and the error code
    iretq

section .rodata
global isr64_stubs
isr64_stubs:
%assign i 0
%rep 32
    dq isr %+ i
%assign i i + 1
%endrep
