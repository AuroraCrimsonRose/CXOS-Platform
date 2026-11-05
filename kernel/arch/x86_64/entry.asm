; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /kernel/arch/x86_64/entry.asm  -  the x86-64 kernel's first instruction
; Aurora Tejeda / CATX Systems
;
; Stage 2 (boot/long.asm) jumps here in 64-bit mode, paging on with its own
; tables (the first GiB at 0 and at KERNEL_VBASE), interrupts off, RDI the
; physical address of the CXBI handover. The loader zeroed .bss as the tail of
; its segment, so nothing here needs to. Move to the kernel's own stack, which
; is in the top 2 GiB like the rest of the image, and call kmain (kernel/kmain.c), as x86-32 does.
;
; Before kmain, the GS base is bound to the boot CPU's own data (cpu/percpu.h),
; so the first spinlock or thread id finds it rather than whatever the loader
; left. arch_cpu_init binds it again after the GDT's segment loads, which zero
; it. The base swapgs swaps in (ring 3's) starts at 0.

bits 64
default rel

%define MSR_GS_BASE        0xC0000101   ; gdt.h
%define MSR_KERNEL_GS_BASE 0xC0000102

section .text.entry
global _start
extern kmain
extern cpu_boot

_start:
    cli
    lea rsp, [boot_stack_top]
    xor ebp, ebp                    ; the end of every stack walk
    lea rax, [cpu_boot]
    mov rdx, rax
    shr rdx, 32
    mov ecx, MSR_GS_BASE
    wrmsr
    xor eax, eax
    xor edx, edx
    mov ecx, MSR_KERNEL_GS_BASE
    wrmsr
    call kmain                      ; RDI is still the CXBI address
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
boot_stack:
    resb 16384
boot_stack_top:
