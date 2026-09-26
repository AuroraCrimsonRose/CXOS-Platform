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
;
; ---- Notes on booting real hardware (as opposed to an emulator) ------------
;
; Three details below exist purely because real BIOSes are pickier than QEMU,
; and getting any of them wrong gives the same useless symptom: a black screen
; with a blinking cursor and no output at all, because the BIOS never hands
; control over and none of this code ever runs.
;
;  1. The first three bytes must NOT be EB xx 90. That byte pattern is the
;     FAT12/16 boot-sector signature (`jmp short` / `nop`), and BIOSes that see
;     it treat the medium as a FAT volume and parse a BIOS Parameter Block at
;     offset 3. There is no BPB here, so what they parse is garbage and many
;     will refuse to boot. Stage 1 therefore starts with real code and keeps
;     the Disk Address Packet at the end. Nothing needs the DAP at a fixed
;     offset: `mov si, dap` resolves it symbolically and no tool patches it.
;
;  2. The stack must not sit at the top of conventional memory. The old
;     0x9000:0xFFFF put it at linear 0x9FFFF, which on real machines is inside
;     the Extended BIOS Data Area - so the first BIOS call that used the EBDA
;     fought with our stack. QEMU has effectively no EBDA, which is why this
;     never showed up in emulation. 0x0000:0x7C00 grows down into the free
;     0x00500-0x07BFF window instead, well clear of both the EBDA and us.
;
;  3. LBA (INT 13h extended read) support is checked rather than assumed, so a
;     machine without it says so instead of failing in a way that looks
;     identical to the two cases above.
;
; A fourth requirement lives outside this file: a real MBR partition table,
; with one entry marked active, is written at offset 446 by the image writer
; (CXEX.Build/Emitters/XBPTImageWriter.cs). BIOSes booting USB media in
; USB-HDD mode look for it, and an all-zero table makes them fall back to
; floppy emulation - straight back into problem 1.

org 0x7C00
bits 16

STAGE2_SEGMENT  equ 0x0000
STAGE2_OFFSET   equ 0x7E00      ; load stage 2 right after the boot sector
STAGE2_LBA      equ 2           ; stage 2 starts at LBA 2 (LBA 1 holds the XBPT)
STAGE2_SECTORS  equ 32          ; 16 KB of headroom for stage 2
STACK_TOP       equ 0x7C00      ; grows DOWN from just below this code

; Execution starts here, at the very first byte. No leading jump: see note 1.
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax                  ; MOV SS inhibits interrupts for one
    mov sp, STACK_TOP           ; instruction, so this pair is atomic
    sti

    mov [boot_drive], dl        ; BIOS leaves the boot drive in DL

    ; ---- check INT 13h extensions are present on THIS drive (note 3) ----
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .no_lba                  ; CF set => extensions not supported
    cmp bx, 0xAA55              ; BX flipped => extensions present
    jne .no_lba

    ; ---- read stage 2 ----
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap
    int 0x13
    jc .disk_error

    jmp STAGE2_SEGMENT:STAGE2_OFFSET

.no_lba:
    mov si, msg_nolba
    jmp .print
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

; Disk Address Packet for INT 13h / AH=42h (extended read). Kept after the
; code so the sector does not open with the FAT signature (note 1).
dap:
    db 0x10                     ; DAP size
    db 0x00                     ; reserved
    dw STAGE2_SECTORS           ; sectors to read
    dw STAGE2_OFFSET            ; destination offset
    dw STAGE2_SEGMENT           ; destination segment
    dq STAGE2_LBA               ; starting LBA

boot_drive  db 0
msg_err     db '[BOOT] STAGE2 READ ERROR', 0
msg_nolba   db '[BOOT] NO INT13H LBA SUPPORT', 0

; ----------------------------------------------------------------------------
; Copyright embedded in the boot-sector BINARY (sits in what would be zero
; padding, so it costs no usable space yet shows in strings/hexdump).
; It must stop short of offset 446, where the MBR partition table lives.
; ----------------------------------------------------------------------------
copyright_notice:
    db 'CXK Bootloader v5 - (c) 2026 CATX Systems LLC. '
    db 'CXK/CXOS Project License v1.0.7. All rights reserved.', 0

; Fail the build rather than ship a boot sector that overruns the partition
; table: everything above must end before offset 446 (0x1BE).
%if ($-$$) > 446
    %error "stage 1 overruns the MBR partition table at offset 446"
%endif

times 446-($-$$) db 0           ; pad to the partition table
times 64         db 0           ; partition table: filled in by the image writer
dw 0xAA55
