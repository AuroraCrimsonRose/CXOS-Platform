; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /kernel/arch/x86_64/switch.asm  -  context_switch (arch/cpu.h)
; Aurora Tejeda / CATX Systems
;
; void context_switch(uintptr_t *old_sp, uintptr_t new_sp)
;   rdi = where to save the running thread's RSP, rsi = the next thread's.
;
; Saves what the SysV ABI says a callee keeps - rbx, rbp, r12-r15 - and
; RFLAGS, so a thread resumes with the interrupt state it switched out with.
; From the saved RSP upward the frame is
;   [r15][r14][r13][r12][rbp][rbx][rflags][return]
; and arch_thread_frame (cpu.c) lays out a new thread's first frame the same
; way; keep the two in step. The frame is 8 quadwords, so a thread switched
; out by a call from 16-byte-aligned code resumes aligned too.

bits 64
default rel

section .text
global context_switch

context_switch:
    pushfq
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp          ; save the running thread's RSP
    mov rsp, rsi            ; and take the next thread's stack

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq

    ret                     ; into the next thread
