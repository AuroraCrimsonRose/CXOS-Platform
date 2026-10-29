; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /boot/pmode.asm  -  enter 32-bit protected mode (stage 2)
; Aurora Tejeda / CATX Systems
;
; Real mode to flat 32-bit protected mode, proved by a mark on VGA; then the
; handover is sealed and long.asm takes the kernel the rest of the way.

; enter_protected_mode - no return; execution continues at pmode_entry
enter_protected_mode:
    cli
    xor ax, ax
    mov ds, ax
    lgdt [gdt_descriptor]        ; load the boot GDT (gdt.asm)

    mov eax, cr0
    or  eax, 0x1                 ; set PE (protection enable)
    mov cr0, eax

    jmp GDT_CODE:pmode_entry     ; far jump flushes the pipeline into 32-bit

bits 32
pmode_entry:
    ; Load the flat 32-bit data segments FIRST. Until DS is reloaded with the
    ; flat selector it still holds the real-mode value (limit 0xFFFF = 64KB), so
    ; ANY write above 64KB - like the VGA marker at 0xB809E - faults with #GP
    ; ("write beyond limit"). QEMU is lax about this; Bochs (correctly) faults
    ; and triple-resets. So: set the segments, THEN touch VGA.
    mov ax, GDT_DATA             ; 0x10 - flat 32-bit data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x9F000             ; 32-bit stack just below video RAM (0xA0000).
                                 ; Clear of int 13h's 32 KB bounce buffer at
                                 ; 0x10000; the kernel image is at 16 MB.

    ; now that DS is flat (4GB limit), the VGA marker write is safe. If this
    ; bright-white 'P' appears at the top-right, 32-bit mode is fully live.
    mov dword [0xB8000 + (79 * 2)], 0x0F50   ; 'P' bright white at top-right

    call vga_print_pmode

    ; the rest of the handover, then seal it (cxbi.asm)
    call cxbi_finish

    ; place the ELF64 kernel, enter long mode and jump to it (long.asm)
    call elf64_load_and_jump     ; no return

    ; not reached
.hang:
    hlt
    jmp .hang

; write a protected-mode banner straight to VGA text memory (no BIOS in pmode).
; Use row 10 (well below the real-mode banner) so the result is unambiguous -
; if we see green text here, the protected-mode switch definitively worked.
vga_print_pmode:
    push eax
    push edi
    push esi
    mov edi, 0xB8000 + (10 * 80 * 2)   ; row 10, col 0
    mov esi, msg_pmode
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0A               ; bright green on black
    stosw
    jmp .loop
.done:
    pop esi
    pop edi
    pop eax
    ret

msg_pmode db 'CXK stage 2 - 32-bit protected mode OK', 0

bits 16                          ; back to 16-bit for anything after this include