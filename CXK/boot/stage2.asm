; ============================================================================
;  CXK - the CXOS Kernel : Bootloader (Stage 2)
;  Copyright (c) 2026 CATX Systems LLC.  All rights reserved.
;  Licensed under the CXK and CXOS Project License v1.0.7. See LICENSE.
;  Author: Aurora Tejeda / CATX Systems LLC
; ============================================================================
; /CXK/boot/stage2.asm - sometimes i wonder if its better to make lots of tiny files that do specific things for readability and suffer through mapping them or if its better to just use a monolitic stage 2. edit: i think i figured it out.
; Aurora Tejeda
org 0x7E00
bits 16

jmp start

; constants
MEM_REGION_SIZE     equ 24
MEM_BASE            equ 0
MEM_LENGTH          equ 8
MEM_TYPE            equ 16
MEM_FLAGS           equ 20

E820_TYPE_USABLE    equ 1
E820_MAX_ENTRIES    equ 64
E820_BUFFER         equ 0x1000
E820_ENTRY_COUNT    equ 0x1600
E820_CLEAN_COUNT    equ 0x1602
E820_CLEAN_BUFFER   equ 0x1620
KERNEL_RAM_BASE_LO  equ 0x1C20
KERNEL_RAM_BASE_HI  equ 0x1C24
KERNEL_RAM_SIZE_LO  equ 0x1C28
KERNEL_RAM_SIZE_HI  equ 0x1C2C
boot_drive          equ 0x1C30
disk_error_code     equ 0x1C32

STACK_SEGMENT       equ 0x9000
STACK_TOP           equ 0xFFFF
VGA_SEGMENT         equ 0xB800

BOOT_ROW            equ 7
BOOT_COL            equ 21

; includes
%include "print.asm"
%include "error.asm"
%include "a20.asm"
%include "mem.asm"
%include "gdt.asm"
%include "kernel_load.asm"     ; before pmode — needs real mode INT 13h
%include "vbe.asm"             ; before pmode — needs real mode INT 0x10
%include "pmode.asm"

cursor_correction:
    mov ah, 0x02
    mov bh, 0x00
    mov dh, BOOT_ROW
    mov dl, BOOT_COL
    int 0x10
    ret

; entry point
start:
    cli

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ax, STACK_SEGMENT
    mov ss, ax
    mov sp, STACK_TOP
    sti

    call print_clr
    call enable_a20

    mov si, msg_m1
    
    call print_string
    call init_memory

    mov si, msg_m2          

    call print_string       
    call process_e820
    call find_largest_region
    call print_newline

    mov si, msg_raw
    
    call print_string
    
    xor eax, eax
    mov ax, [E820_ENTRY_COUNT]
    
    call print_hex32
    call print_newline

    mov si, msg_clean
    
    call print_string
    
    xor eax, eax
    mov ax, [E820_CLEAN_COUNT]

    call print_hex32
    call debug_memory

    mov si, msg_drv
    call print_string
    mov al, [boot_drive]

    call print_error_code
    call load_kernel            ; load kernel while still in real mode
    call cursor_correction
    call set_video_mode         ; set VBE graphics mode (real mode only); falls
                                ; back to text mode if no match (VBE_VALID=0)
    call enter_protected_mode   ; no return to real mode after this
    jmp halt                    ; never reached

; halt
halt:
    hlt
    jmp $

; strings
msg_base        db '[BOOT] MEM BASE: ', 0
msg_size        db '[BOOT] MEM SIZE: ', 0
msg_no_mem      db '[BOOT] FATAL NO MEMORY', 0
msg_a20_ok      db '[BOOT] A20 OK', 0
msg_a20_fail    db '[BOOT] A20 FAIL', 0
msg_raw         db '[BOOT] RAW=', 0
msg_clean       db '[BOOT] CLEAN=', 0
msg_m1          db '[BOOT] TEST ', 0
msg_m2          db 'PASS', 0
msg_drv         db '[BOOT] DRIVE=', 0