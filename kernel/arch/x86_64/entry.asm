; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /kernel/arch/x86_64/entry.asm  -  the x86-64 kernel's first instruction
; Aurora Tejeda / CATX Systems
;
; Stage 2 (boot/long.asm) jumps here in 64-bit mode, paging on with its own
; tables (the first GiB at 0 and at KERNEL_VBASE), interrupts off, RDI the
; physical address of the CXBI handover. The loader zeroed .bss as the tail of
; its segment, so nothing here needs to. Move to the kernel's own stack, which
; is in the top 2 GiB like the rest of the image, and call boot64_main.

bits 64
default rel

section .text.entry
global _start
extern boot64_main

_start:
    cli
    lea rsp, [boot_stack_top]
    xor ebp, ebp                    ; the end of every stack walk
    call boot64_main                ; RDI is still the CXBI address
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
boot_stack:
    resb 16384
boot_stack_top:
