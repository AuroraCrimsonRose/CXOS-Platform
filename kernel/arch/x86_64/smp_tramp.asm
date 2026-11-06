; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /kernel/arch/x86_64/smp_tramp.asm  -  where an AP starts (1.9d)
; Aurora Tejeda / CATX Systems
;
; A STARTUP IPI starts an AP in real mode at page:0 (CS = page << 8, IP 0),
; so this is copied to SMP_TRAMP_PHYS, a page of the first MiB (smp.c), and
; never runs where it is linked: it lives in .rodata. Every address in it is
; that page's, written as T(label). From real mode to protected mode on a
; GDT of its own, then PAE, the root smp.c built (the kernel half and this
; page, below 4 GiB, as a 32-bit mov to CR3 needs), EFER.LME and NXE (the
; kernel's tables set the no-execute bit, which is reserved without NXE),
; and the boot CPU's CR0, which turns paging on: long mode. A far jump to
; its 64-bit code, then the AP's own stack and struct cpu, and smp_ap_entry,
; which never returns. Real mode to protected first, as Serenity's
; apic_ap_start and Haiku's trampoline do, rather than straight to long mode.
;
; smp.c writes the six quadwords at the end before each STARTUP and starts
; one AP at a time, so one page serves them all.

%define SMP_TRAMP_PHYS 0x8000       ; smp.c

default abs                         ; every address here is the page's own
%define T(x) (SMP_TRAMP_PHYS + (x) - smp_tramp_start)

section .rodata
align 16
global smp_tramp_start, smp_tramp_end
global smp_tramp_cr0, smp_tramp_cr3, smp_tramp_cr4
global smp_tramp_stack, smp_tramp_cpu, smp_tramp_entry

bits 16
smp_tramp_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    lgdt [T(tr_gdtr)]
    mov eax, cr0
    or eax, 1                       ; PE
    mov cr0, eax
    jmp dword 0x08:T(tr_prot)

bits 32
tr_prot:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [T(smp_tramp_cr4)]     ; the boot CPU's: PAE among them
    mov cr4, eax
    mov eax, [T(smp_tramp_cr3)]
    mov cr3, eax
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, (1 << 8) | (1 << 11)    ; LME, NXE
    wrmsr
    mov eax, [T(smp_tramp_cr0)]     ; PG, PE, WP: long mode, compatibility
    mov cr0, eax
    jmp 0x18:T(tr_long)

bits 64
tr_long:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [T(smp_tramp_stack)]   ; 16-byte aligned: the call leaves 8 below
    mov rdi, [T(smp_tramp_cpu)]
    mov rax, [T(smp_tramp_entry)]
    xor ebp, ebp                    ; the end of every stack walk
    call rax
    ud2                             ; smp_ap_entry never returns

align 8
tr_gdt:
    dq 0
    dq 0x00CF9A000000FFFF           ; 0x08: 32-bit code, flat
    dq 0x00CF92000000FFFF           ; 0x10: data, flat
    dq 0x00AF9A000000FFFF           ; 0x18: 64-bit code
tr_gdtr:
    dw 4 * 8 - 1
    dd T(tr_gdt)

align 8
smp_tramp_cr0:   dq 0
smp_tramp_cr3:   dq 0               ; the boot root: below 4 GiB
smp_tramp_cr4:   dq 0
smp_tramp_stack: dq 0               ; the AP's idle stack's top
smp_tramp_cpu:   dq 0               ; its struct cpu
smp_tramp_entry: dq 0               ; smp_ap_entry
smp_tramp_end:
