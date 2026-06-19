; ============================================================================
;  CXK - the CXOS Kernel : Bootloader (Stage 2)  [v5]
;  Copyright (c) 2026 CATX Systems LLC.  All rights reserved.
;
;  This file is part of the CXK / CXOS Project and is licensed under the
;  CXK and CXOS Project License, Version 1.0.7 (Effective June 16, 2026).
;  Author: Aurora Tejeda / CATX Systems LLC
; ============================================================================
; /CXK/boot/stage2.asm - Stage 2
;
; Increment 2: get the CPU into flat 32-bit protected mode.
;   real mode -> enable A20 -> load boot GDT -> set CR0.PE -> 32-bit pmode.
; Per design (b), stage 2 stops at "clean flat 32-bit protected mode"; the
; kernel's own entry stub will later set up paging and map itself into the
; higher half. Stage 2 does NOT do paging/higher-half or build the kernel's
; real GDT/TSS (those are kernel-owned).

org 0x7E00              ; stage 1 loads us here
bits 16

stage2_start:
    mov [boot_drive_s2], dl     ; stage 1 left the boot drive in DL
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov si, msg_banner
    call print_string
    call print_newline

    call enable_a20             ; a20.asm (prints its own OK/!fail)

    call gather_memory_map      ; mem.asm - MUST be before pmode (needs BIOS)
    call print_mem_count        ; show how many E820 entries we got

    call load_kernel            ; read kernel image from disk to 0x100000 (BIOS)

    ; LAST real-mode step: set a VBE linear-framebuffer graphics mode (needs BIOS
    ; int 10h, gone after the pmode switch). Self-gates on CXK_ENABLE_FB: when 0
    ; it just clears VBE_VALID and leaves the display in text mode. On success it
    ; prints the chosen mode BEFORE switching (text is invisible once graphics is
    ; live), then the kernel reads the info struct via fb_init. Comes after all
    ; other logging so the boot text above stays visible.
    call set_vbe_mode

    call enter_protected_mode   ; pmode.asm - no return (ends jumping to kernel)

    ; not reached
    jmp halt

; ----------------------------------------------------------------------------
; load_kernel - read the kernel image from disk into a LOW buffer (real mode,
; BIOS int 13h). The kernel is later copied up to physical 0x100000 by the
; protected-mode code (32-bit addressing makes that trivial and avoids the
; fragile real-mode >1MB segment tricks). The kernel lives on disk right after
; stage 2: stage 1 = LBA 0, stage 2 = LBA 1..32, so the kernel starts at LBA 33.
; We load it to 0x10000 (linear), a free low buffer.
KERNEL_LOAD_LOW  equ 0x10000        ; temp buffer (segment 0x1000:0x0000)
KERNEL_DEST_HIGH equ 0x100000       ; final physical location (1 MB)
KERNEL_SECTORS   equ 512            ; 256 KB of headroom for the kernel image.
                                    ; Must fit in LOW memory (the 0x10000 buffer
                                    ; lives below 1MB, with the pmode stack at
                                    ; 0x9F000) - 256KB ends at 0x50000, leaving a
                                    ; ~316KB gap before the stack. BIOS int13h
                                    ; can't read this in one call (~127-sector
                                    ; limit), so load_kernel loops in chunks.
                                    ; cxexload copies only each section's real
                                    ; bytes, so over-reading empty tail sectors
                                    ; is harmless. (Kernel is ~50KB now: 5x room.)
KERNEL_CHUNK     equ 64             ; sectors per int13h call (<=127, BIOS-safe).
                                    ; 64*512 = 32 KB = 0x800 segment units/chunk.
KERNEL_CHUNKS    equ (KERNEL_SECTORS / KERNEL_CHUNK)   ; 512/64 = 8

load_kernel:
    push es
    push cx
    mov cx, KERNEL_CHUNKS           ; number of chunks to read
.next_chunk:
    push cx
    mov word [dap_kernel + 2], KERNEL_CHUNK  ; re-arm count (some BIOSes write
                                             ; back the actual-read count here)
    mov ah, 0x42
    mov dl, [boot_drive_s2]
    mov si, dap_kernel
    int 0x13
    jc .kerr
    pop cx

    ; advance the DAP for the next chunk:
    ;   dest segment += 0x800  (64 sectors * 512 = 32 KB = 0x800 paragraphs)
    ;   start LBA     += 64
    add word [dap_kernel + 6], 0x800        ; dest segment field (offset 6)
    add dword [dap_kernel + 8], KERNEL_CHUNK ; LBA low dword (offset 8)
    adc dword [dap_kernel + 12], 0           ; carry into LBA high dword

    loop .next_chunk                ; cx-- ; jump if cx != 0

    pop cx
    pop es
    ret
.kerr:
    mov si, msg_kerr
    call print_string
    jmp halt

dap_kernel:
    db 0x10
    db 0x00
    dw KERNEL_CHUNK             ; sectors per read (one chunk)
    dw 0x0000                   ; dest offset
    dw 0x1000                   ; dest segment -> 0x10000 linear (advances per chunk)
    dq 33                       ; LBA 33 (right after 32-sector stage 2); advances

boot_drive_s2 db 0
msg_kerr db '[BOOT] KERNEL READ ERROR', 0

; print "[BOOT] E820 entries: N" (N as a small decimal, real mode)
print_mem_count:
    push ax
    push si
    mov si, msg_e820
    call print_string
    mov ax, [E820_COUNT_ADDR]
    call print_dec_ax
    call print_newline
    pop si
    pop ax
    ret

; print AX as unsigned decimal (real-mode BIOS teletype)
print_dec_ax:
    push ax
    push bx
    push cx
    push dx
    mov bx, 10
    xor cx, cx                  ; digit count
.divloop:
    xor dx, dx
    div bx                      ; AX = AX/10, DX = remainder
    push dx
    inc cx
    test ax, ax
    jnz .divloop
.printloop:
    pop dx
    add dl, '0'
    mov al, dl
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    loop .printloop
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ----------------------------------------------------------------------------
; real-mode helpers (used by stage2 + a20.asm)
; ----------------------------------------------------------------------------
; print_string: SI -> NUL-terminated string, BIOS teletype
print_string:
    push ax
    push bx
.next:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    jmp .next
.done:
    pop bx
    pop ax
    ret

print_newline:
    push ax
    mov ah, 0x0E
    mov al, 13
    int 0x10
    mov al, 10
    int 0x10
    pop ax
    ret

halt:
    cli
    hlt
    jmp halt

msg_banner db 'CXK v5 stage 2 - entering pmode', 0
msg_e820   db '[BOOT] E820 entries: ', 0

; a20 messages (referenced by a20.asm)
msg_a20_ok   db '[BOOT] A20 OK', 0
msg_a20_fail db '[BOOT] A20 FAIL', 0

; ----------------------------------------------------------------------------
; includes (order matters): A20 logic, memory map, the boot GDT data, pmode.
; ----------------------------------------------------------------------------
%include "a20.asm"
%include "mem.asm"
%include "gdt.asm"
%include "pmode.asm"
%include "cxexload.asm"
%include "vbe.asm"

; embedded copyright (binary)
copyright_notice:
    db 'CXK Stage2 v5 - (c) 2026 CATX Systems LLC. '
    db 'CXK/CXOS Project License v1.0.7. All rights reserved.', 0