; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /kernel/arch/x86_64/isr.asm  -  the exception and interrupt entry stubs
; Aurora Tejeda / CATX Systems
;
; One stub per vector 0-56: the exceptions 0-31, the legacy IRQs 32-47,
; the MSI pool 48-55 and the local APIC timer at 56 (idt.h, apic.h). Each
; makes the frame uniform - a zero where the CPU pushes no error code, then
; the vector - saves the general
; registers and calls idt64_dispatch(struct registers *). Whatever the
; handler leaves in the frame is what iretq resumes, RIP and RSP included.
; The layout is struct registers in idt.h; keep the two in step.
;
; In 64-bit mode the CPU aligns RSP to 16 before it pushes its frame, so the
; 5 + 2 + 15 = 22 quadwords below leave the call 16-byte aligned, as the
; SysV ABI wants.
;
; The local APIC's spurious vector has a stub of its own that only returns:
; a spurious interrupt sets no in-service bit, so it takes no EOI.
;
; GS (cpu/percpu.h, usermode.asm): an interrupt from ring 3 arrives with ring
; 3's GS base, so isr_common swaps in the CPU's when the frame's CS is ring
; 3's, and back before the iretq to it. That test is wrong for the IST
; vectors (#DB, NMI, #DF, #MC), which can land in the kernel between a
; syscall and its swapgs, or a swapgs and its sysret: CS is the kernel's and
; GS ring 3's. Their stubs go to isr_paranoid, which asks the GS MSR instead,
; as XNU's idt64_debug does (osfmk/x86_64/idt64.s, the MSR's top bit) and
; Linux's paranoid_entry (from memory). The kernel's base is in
; the top half and ring 3's never is (no FSGSBASE: ring 3 sets it only by
; loading a selector, from a 32-bit descriptor base), so its sign says whose
; it is.

bits 64
default rel

section .text
extern idt64_dispatch

%define MSR_GS_BASE 0xC0000101     ; gdt.h

; The second argument is where the stub goes: isr_paranoid for the IST vectors.
%macro ISR_NOERR 1-2 isr_common
isr%1:
    push 0
    push %1
    jmp %2
%endmacro

%macro ISR_ERR 1-2 isr_common
isr%1:
    push %1                         ; the CPU pushed the error code
    jmp %2
%endmacro

ISR_NOERR 0
ISR_NOERR 1, isr_paranoid      ; #DB
ISR_NOERR 2, isr_paranoid      ; NMI
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8, isr_paranoid      ; #DF
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18, isr_paranoid     ; #MC
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

%assign i 32
%rep 25
isr %+ i:
    push 0
    push i
    jmp isr_common
%assign i i + 1
%endrep

global isr64_spurious
isr64_spurious:
    iretq

%macro PUSH_GPRS 0
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
%endmacro

%macro POP_GPRS 0
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
%endmacro

; [rsp] the vector, +8 the error code, +16 RIP, +24 CS.
isr_common:
    test byte [rsp + 24], 3         ; from ring 3: its GS base is loaded
    jz .gs_kernel
    swapgs
.gs_kernel:
    PUSH_GPRS
    mov rdi, rsp
    cld
    call idt64_dispatch
    POP_GPRS
    add rsp, 16                     ; the vector and the error code
    test byte [rsp + 8], 3          ; back to ring 3: its GS base again
    jz .gs_stays
    swapgs
.gs_stays:
    iretq

; The IST vectors: whose GS is loaded comes from the MSR (the top of this
; file). rbx, saved in the frame and kept by the call, remembers a swap.
isr_paranoid:
    PUSH_GPRS
    mov ecx, MSR_GS_BASE
    rdmsr
    xor ebx, ebx
    test edx, edx                   ; the top bit: the kernel's base
    js .gs_kernel
    swapgs
    mov ebx, 1
.gs_kernel:
    mov rdi, rsp
    cld
    call idt64_dispatch
    test ebx, ebx
    jz .gs_stays
    swapgs                          ; whoever was interrupted gets its own back
.gs_stays:
    POP_GPRS
    add rsp, 16
    iretq

section .rodata
global isr64_stubs
isr64_stubs:
%assign i 0
%rep 57
    dq isr %+ i
%assign i i + 1
%endrep
