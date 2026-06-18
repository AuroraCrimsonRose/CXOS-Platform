; ============================================================================
;  CXK - the CXOS Kernel : Bootloader (Stage 1)
;  Copyright (c) 2026 CATX Systems LLC.  All rights reserved.
;
;  This file is part of the CXK / CXOS Project and is licensed under the
;  CXK and CXOS Project License, Version 1.0.7 (Effective June 16, 2026).
;  Use of this software is subject to the terms of that License. See the
;  LICENSE file distributed with the Project for the full terms.
;
;  Author: Aurora Tejeda / CATX Systems LLC
; ============================================================================
; /CXK/boot/boot.asm - Stage 1
; Aurora Tejeda

org 0x7C00
bits 16

STAGE2_SEGMENT  equ 0x0000
STAGE2_OFFSET   equ 0x7E00
STAGE2_SECTORS  equ 16
STACK_SEGMENT   equ 0x9000
STACK_TOP       equ 0xFFFF
BOOT_DRIVE      equ 0x1C30

jmp start
nop                         ; pad to 3 bytes so DAP sits at 0x7C03

; DAP at fixed offset — SI will point here reliably
dap:
    db 0x10
    db 0x00
    dw STAGE2_SECTORS
    dw STAGE2_OFFSET
    dw STAGE2_SEGMENT
    dq 1                    ; LBA sector 1 = stage 2

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ax, STACK_SEGMENT
    mov ss, ax
    mov sp, STACK_TOP
    sti

    mov [BOOT_DRIVE], dl

    ; check LBA extensions — dl must be set
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc .no_lba
    cmp bx, 0xAA55
    jne .no_lba

    ; extended read
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    mov si, dap
    int 0x13
    jc .disk_error
    jmp STAGE2_SEGMENT:STAGE2_OFFSET

.no_lba:
    mov si, msg_no_lba
    jmp .print_halt

.disk_error:
    mov si, msg_disk_err

.print_halt:
.loop:
    lodsb
    test al, al
    jz .hang
    mov ah, 0x0E
    mov bh, 0
    int 0x10
    jmp .loop

.hang:
    jmp $

msg_disk_err    db '[BOOT] DISK ERR', 0
msg_no_lba      db '[BOOT] NO LBA', 0

; ----------------------------------------------------------------------------
; Copyright notice embedded in the boot-sector BINARY (not just source).
; This sits in what would otherwise be zero padding, so it costs no usable
; space, yet `strings`/hexdump of the boot image will always show it.
; ----------------------------------------------------------------------------
copyright_notice:
    db 'CXK Bootloader - (c) 2026 CATX Systems LLC. '
    db 'CXK/CXOS Project License v1.0.7. All rights reserved.', 0

times 510-($-$$) db 0
dw 0xAA55