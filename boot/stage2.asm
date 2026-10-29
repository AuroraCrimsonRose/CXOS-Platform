; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; ============================================================================
;  CXK - the CXOS Kernel : Bootloader (Stage 2)  [v5]
;  Copyright (c) 2026 CATX Systems.  All rights reserved.
;
;  This file is part of the CXK / CXOS Project and is licensed under the
;  CXK and CXOS Project License, Version 1.0.7 (Effective June 16, 2026).
;  Author: Aurora Tejeda / CATX Systems
; ============================================================================
; /boot/stage2.asm - Stage 2
;
; Gets the CPU from real mode to the x86-64 kernel:
;   real mode -> enable A20 -> load boot GDT -> set CR0.PE -> 32-bit pmode
;   -> long.asm: place the ELF64 kernel, build the minimal page tables long
;   mode cannot run without, enter long mode and jump to it.
; Stage 2 builds only those tables; the kernel replaces them with its own,
; and its GDT and TSS are its own too.

org 0x7E00              ; stage 1 loads us here
bits 16

%ifndef CXK_TEST_HOSTILE_BIOS
%define CXK_TEST_HOSTILE_BIOS 0
%endif

stage2_start:
    mov [boot_drive_s2], dl     ; stage 1 left the boot drive in DL
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov si, msg_banner
    call print_string
    call print_newline

    call enable_a20             ; a20.asm (prints its own OK/!fail)

    call cxbi_clear             ; cxbi.asm - mem.asm and vbe.asm fill it from here

    call gather_memory_map      ; mem.asm - MUST be before pmode (needs BIOS)
    call print_mem_count        ; show how many E820 entries we got

    call load_kernel            ; read the kernel image to 16 MB (BIOS + unreal mode)

    ; LAST real-mode step: set a VBE linear-framebuffer graphics mode (needs BIOS
    ; int 10h, gone after the pmode switch). Self-gates on CXK_ENABLE_FB: when 0
    ; it just leaves CXBI's fb_addr 0 and the display in text mode. On success it
    ; prints the chosen mode BEFORE switching (text is invisible once graphics is
    ; live), and the mode goes into CXBI for the kernel. Comes after all
    ; other logging so the boot text above stays visible.
    call set_vbe_mode

    call enter_protected_mode   ; pmode.asm - no return (ends jumping to kernel)

    ; not reached
    jmp halt

; ----------------------------------------------------------------------------
; load_kernel - read the kernel image from the BOOT partition and gather it at
; 16 MB (real mode, BIOS int 13h). long.asm later places its segments at their
; physical addresses, from 1 MB up, below the gathered image.
;
; int 13h can only write below 1 MB, so each chunk lands in a 32 KB bounce
; buffer at 0x10000 and is copied up from there in unreal mode: FS carries a
; 4 GB limit while the CPU stays in real mode with the BIOS usable. That limit
; lives in the descriptor cache, and a BIOS may reload it - firmware whose disk
; code passes through protected mode can leave 64 KB limits behind, Intel and
; AMD boards alike - so unreal_enter runs again after EVERY int 13h, not once.
; CXK_TEST_HOSTILE_BIOS builds a stage 2 that plays that BIOS.
;
; Until 1.3a' the whole image sat below 1 MB, from 0x10000 to the pmode stack at
; 0x9F000; 544 KB was the most that held, and the kernel had reached 517 KB.
KERNEL_LOAD_LOW  equ 0x10000        ; int 13h's bounce buffer (0x1000:0x0000)
KERNEL_LOAD_HIGH equ 0x1000000      ; where the image is gathered: 16 MB, checked
                                    ; against the E820 map before the first read
KERNEL_SECTORS   equ 8192           ; 4 MB: the most stage 2 will read.
                                    ;
                                    ; The image writer patches the kernel's real
                                    ; size into kernel_sectors below and refuses a
                                    ; kernel larger than this budget
                                    ; (Stage2KernelSectorBudget in
                                    ; XBPTImageWriter.cs; a DevKit test holds the
                                    ; two equal). The budget was once a silent hard
                                    ; cap: a kernel past it lost the tail of its
                                    ; image and the only symptom was one unrelated
                                    ; self-test failing.
KERNEL_CHUNK     equ 64             ; sectors per int13h call (<=127, BIOS-safe).
CHUNK_BYTES      equ KERNEL_CHUNK * 512                ; 32 KB

load_kernel:
    call find_boot_partition        ; read XBPT, point dap_kernel at the BOOT
                                    ; partition's start LBA (overrides the
                                    ; default below)
    ; whole chunks covering the kernel; a count outside the budget is refused
    mov eax, [kernel_sectors]
    test eax, eax
    jz .kbig
    cmp eax, KERNEL_SECTORS
    ja .kbig
    add eax, KERNEL_CHUNK - 1
    shr eax, 6                      ; / KERNEL_CHUNK
    mov [kernel_chunks], ax
    shl eax, 15                     ; * CHUNK_BYTES
    mov [kernel_read_bytes], eax    ; long.asm bounds the ELF by this
    call check_high_buffer          ; [16 MB, + read bytes) is usable RAM
    mov dword [kernel_dest], KERNEL_LOAD_HIGH
.next_chunk:
    mov word [dap_kernel + 2], KERNEL_CHUNK  ; re-arm count (some BIOSes write
                                             ; back the actual-read count here)
    mov ah, 0x42
    mov dl, [boot_drive_s2]
    mov si, dap_kernel
    int 0x13
    jc .kerr
%if CXK_TEST_HOSTILE_BIOS
    call hostile_bios_limits        ; test builds: the BIOS reset FS's limit
%endif
    call unreal_enter               ; after every int 13h (see above)

    ; copy the chunk up: FS has base 0 and a 4 GB limit
    mov esi, KERNEL_LOAD_LOW
    mov edi, [kernel_dest]
    mov ecx, CHUNK_BYTES / 4
.copy:
    mov eax, [fs:esi]
    mov [fs:edi], eax
    add esi, 4
    add edi, 4
    dec ecx
    jnz .copy
    mov [kernel_dest], edi

    add dword [dap_kernel + 8], KERNEL_CHUNK ; LBA low dword (offset 8)
    adc dword [dap_kernel + 12], 0           ; carry into LBA high dword
    dec word [kernel_chunks]
    jnz .next_chunk
    ret
.kerr:
    mov si, msg_kerr
    call print_string
    jmp halt
.kbig:
    mov si, msg_kbig
    call print_string
    jmp halt

dap_kernel:
    db 0x10
    db 0x00
    dw KERNEL_CHUNK             ; sectors per read (one chunk)
    dw 0x0000                   ; dest offset
    dw 0x1000                   ; dest segment -> 0x10000, the bounce buffer
    dq 33                       ; start LBA - DEFAULT only; find_boot_partition
                                ; overwrites this with the BOOT partition's
                                ; start_lba read from the XBPT at LBA 1.

; The kernel's size in sectors. The image writer finds the 'TNSK' marker and
; patches the dword after it (XBPTImageWriter.PatchStage2KernelSectors); an
; unpatched stage 2 reads the whole budget.
                  db 'TNSK'
kernel_sectors    dd KERNEL_SECTORS
kernel_chunks     dw 0
kernel_read_bytes dd 0                  ; what load_kernel gathered at 16 MB
kernel_dest       dd 0

boot_drive_s2 db 0
msg_kerr db '[BOOT] KERNEL READ ERROR', 0
msg_kbig db '[BOOT] KERNEL SIZE OUTSIDE THE LOADER BUDGET', 0
msg_khigh db '[BOOT] NO USABLE RAM AT 16 MB FOR THE KERNEL IMAGE', 0

; ----------------------------------------------------------------------------
; check_high_buffer - halt unless one usable E820 entry covers
; [KERNEL_LOAD_HIGH, KERNEL_LOAD_HIGH + kernel_read_bytes). Real mode, after
; gather_memory_map, while the CXBI map still holds the raw E820 types.
check_high_buffer:
    pushad
    mov cx, [CXBI_ADDR + CXBI_O_MMAP_COUNT]
    mov si, CXBI_MMAP
    mov ebx, KERNEL_LOAD_HIGH
    add ebx, [kernel_read_bytes]        ; the end wanted, below 4 GB
.next:
    test cx, cx
    jz .none
    cmp dword [si + 16], 1              ; E820 type 1: usable
    jne .skip
    cmp dword [si + 4], 0               ; base below 4 GB ...
    jne .skip
    cmp dword [si], KERNEL_LOAD_HIGH    ; ... and at or below 16 MB
    ja .skip
    mov eax, [si]
    xor edx, edx
    add eax, [si + 8]                   ; end = base + length, 64-bit
    adc edx, [si + 12]
    jnz .ok                             ; ends past 4 GB
    cmp eax, ebx
    jae .ok
.skip:
    add si, E820_ENTRY_SIZE
    dec cx
    jmp .next
.ok:
    popad
    ret
.none:
    mov si, msg_khigh
    call print_string
    jmp halt

; ----------------------------------------------------------------------------
; unreal_enter - FS and GS get base 0 and a 4 GB limit while the CPU stays in
; real mode: protected mode for long enough to load them from the boot GDT's
; flat data descriptor, then back. Real-mode loads of FS and GS change only the
; base, so the limit stays until something reloads it in protected mode.
unreal_enter:
    push bx
    mov bx, GDT_DATA
    call load_fs_gs_limit
    pop bx
    ret

; load_fs_gs_limit - BX = a boot GDT data selector. FS and GS take its limit,
; and base 0. Interrupts are off across the switch (an interrupt in protected
; mode would go through an IDT that is not there), then as they were.
load_fs_gs_limit:
    pushf
    push eax
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short .pm                       ; flush the prefetch queue
.pm:
    mov fs, bx
    mov gs, bx
    and al, 0xFE
    mov cr0, eax
    jmp short .rm
.rm:
    xor ax, ax
    mov fs, ax
    mov gs, ax
    pop eax
    popf
    ret

%if CXK_TEST_HOSTILE_BIOS
; hostile_bios_limits - TEST BUILDS ONLY (cxk_flags.cmake). Leaves FS and GS
; with 64 KB limits after an int 13h, as a BIOS whose disk code passes through
; protected mode may. The boot must still pass: unreal_enter after every int
; 13h is what makes it. With unreal mode entered only once, the first copy
; above 64 KB faults and the boot never reaches the kernel.
hostile_bios_limits:
    push bx
    mov bx, GDT_DATA16
    call load_fs_gs_limit
    pop bx
    ret
%endif

; ----------------------------------------------------------------------------
; find_boot_partition - read the XBPT table (LBA 1) and point dap_kernel at the
; CXBOOT partition's start LBA. Real mode (BIOS int 13h still available). Halts
; with a message on read error / bad magic / no boot partition.
;
; XBPT on-disk layout (little-endian, must match drivers/storage/partition +
; tools/mkdisk.py):
;   header (32B): "XBPT"(4) version(2) entry_count(2 @6) entry_size(2 @8)
;                 flags(2) disk_sectors(8) disk_id(8 @20) pad(4)
;   entry  (32B): start_lba(8 @0) sectors(8 @8) type(1 @16) flags(1 @17)
;                 reserved(2) name(12)
; ----------------------------------------------------------------------------
PART_TYPE_CXBOOT equ 0xCB
XBPT_MAGIC_DWORD equ 0x54504258         ; 'X','B','P','T' as a LE dword

find_boot_partition:
    pusha
    ; read 1 sector at LBA 1 -> xbpt_buf
    mov ah, 0x42
    mov dl, [boot_drive_s2]
    mov si, dap_xbpt
    int 0x13
    jc .read_err

    mov si, xbpt_buf
    cmp dword [si], XBPT_MAGIC_DWORD
    jne .bad_magic

    mov cx, [si + 6]                    ; entry_count
    test cx, cx
    jz .no_boot
    lea bx, [si + 32]                   ; bx -> first entry
.scan:
    cmp byte [bx + 16], PART_TYPE_CXBOOT
    je .found
    add bx, 32
    loop .scan
    jmp .no_boot
.found:
    ; copy the 64-bit start_lba into the kernel DAP (offsets 8 = low, 12 = high)
    mov eax, [bx + 0]
    mov [dap_kernel + 8], eax
    mov eax, [bx + 4]
    mov [dap_kernel + 12], eax
    ; and which disk this is, for the kernel (CXBI boot_disk_id): BIOS drive
    ; numbers mean nothing to it, an id read from this very table does
    mov eax, [si + 20]
    mov [CXBI_ADDR + CXBI_O_BOOT_DISK], eax
    mov eax, [si + 24]
    mov [CXBI_ADDR + CXBI_O_BOOT_DISK + 4], eax
    popa
    ret
.read_err:
    mov si, msg_xbpt_rerr
    call print_string
    jmp halt
.bad_magic:
    mov si, msg_xbpt_bad
    call print_string
    jmp halt
.no_boot:
    mov si, msg_xbpt_none
    call print_string
    jmp halt

dap_xbpt:
    db 0x10
    db 0x00
    dw 1                                ; one sector
    dw xbpt_buf                         ; dest offset (segment 0, DS=0)
    dw 0x0000                           ; dest segment
    dq 1                                ; LBA 1 (XBPT)

msg_xbpt_rerr db '[BOOT] XBPT READ ERROR', 0
msg_xbpt_bad  db '[BOOT] XBPT BAD MAGIC', 0
msg_xbpt_none db '[BOOT] NO BOOT PARTITION', 0
xbpt_buf:     times 512 db 0

; print "[BOOT] E820 entries: N" (N as a small decimal, real mode)
print_mem_count:
    push ax
    push si
    mov si, msg_e820
    call print_string
    mov ax, [CXBI_ADDR + CXBI_O_MMAP_COUNT]
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

msg_banner db 'CXK stage 2 - entering pmode', 0
msg_e820   db '[BOOT] E820 entries: ', 0

; a20 messages (referenced by a20.asm)
msg_a20_ok   db '[BOOT] A20 OK', 0
msg_a20_fail db '[BOOT] A20 FAIL', 0

; ----------------------------------------------------------------------------
; includes (order matters): A20 logic, memory map, the boot GDT data, pmode.
; ----------------------------------------------------------------------------
%include "a20.asm"
%include "cxbi.asm"
%include "mem.asm"
%include "gdt.asm"
%include "pmode.asm"
%include "long.asm"
%include "vbe.asm"

; embedded copyright (binary)
copyright_notice:
    db 'CXK Stage2 - (c) 2026 CATX Systems. '
    db 'CXK/CXOS Project License v1.0.7. All rights reserved.', 0