; ============================================================================
;  CXK - the CXOS Kernel : Bootloader (Stage 1)  [v5]
;  Copyright (c) 2026 CATX Systems LLC.  All rights reserved.
;
;  This file is part of the CXK / CXOS Project and is licensed under the
;  CXK and CXOS Project License, Version 1.0.7 (Effective June 16, 2026).
;  Use of this software is subject to the terms of that License. See the
;  LICENSE file distributed with the Project for the full terms.
;
;  Author: Aurora Tejeda / CATX Systems LLC
; ============================================================================
; /CXK/boot/boot.asm - Stage 1 (minimal)
;
; The absolute minimum: set up segments + stack, load stage 2 from a fixed
; LBA via the BIOS extended read, and jump to it. Nothing else - no A20, no
; GDT, no protected mode, no memory map. All of that (and the higher-half
; paging setup) belongs to stage 2. Stage 1 stays tiny and stable because it
; is the one piece that cannot be safely updated in place.

org 0x7C00
bits 16

STAGE2_SEGMENT  equ 0x0000
STAGE2_OFFSET   equ 0x7E00      ; load stage 2 right after the boot sector
STAGE2_LBA      equ 2           ; stage 2 starts at LBA 2 (LBA 1 holds the XBPT)
STAGE2_SECTORS  equ 32          ; 16 KB of headroom for stage 2
STACK_SEGMENT   equ 0x9000
STACK_TOP       equ 0xFFFF

jmp short start
nop                             ; pad so the DAP sits at a fixed offset

; Disk Address Packet for INT 13h / AH=42h (extended read). At a fixed offset
; so SI can point at it reliably.
dap:
    db 0x10                     ; DAP size
    db 0x00                     ; reserved
    dw STAGE2_SECTORS           ; sectors to read
    dw STAGE2_OFFSET            ; destination offset
    dw STAGE2_SEGMENT           ; destination segment
    dq STAGE2_LBA               ; starting LBA

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ax, STACK_SEGMENT
    mov ss, ax
    mov sp, STACK_TOP
    sti

    mov [boot_drive], dl        ; BIOS leaves the boot drive in DL

    ; read stage 2 (extended read; assumes LBA support, which all modern
    ; BIOSes and every machine CXK targets provide)
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap
    int 0x13
    jc .disk_error

    jmp STAGE2_SEGMENT:STAGE2_OFFSET

.disk_error:
    mov si, msg_err
.print:
    lodsb
    test al, al
    jz .hang
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    jmp .print
.hang:
    cli
    hlt
    jmp .hang

boot_drive  db 0
msg_err     db '[BOOT] STAGE2 READ ERROR', 0

; ----------------------------------------------------------------------------
; Copyright embedded in the boot-sector BINARY (sits in what would be zero
; padding, so it costs no usable space yet shows in strings/hexdump).
; ----------------------------------------------------------------------------
copyright_notice:
    db 'CXK Bootloader v5 - (c) 2026 CATX Systems LLC. '
    db 'CXK/CXOS Project License v1.0.7. All rights reserved.', 0

times 510-($-$$) db 0
dw 0xAA55