; /CXK/boot/pmode.asm  -  enter 32-bit protected mode (v5 stage 2)
; Aurora Tejeda / CATX SYSTEMS LLC
;
; Ports v4's pmode switch. Difference for v5: there is no kernel to jump to yet,
; so after entering protected mode we PROVE it (write to VGA) and halt. The
; kernel-load + jump (and the kernel's own higher-half paging) come in later
; increments. Stage 2's job ends at "clean flat 32-bit protected mode".

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
    ; IMMEDIATE proof we reached 32-bit mode: write a marker char to row 0,
    ; col 79 (top-right corner) before anything else. If this single bright-
    ; white 'P' appears, the far jump into protected mode succeeded even if
    ; everything after it somehow fails.
    mov dword [0xB8000 + (79 * 2)], 0x0F50   ; 'P' bright white at top-right

    mov ax, GDT_DATA             ; 0x10 - flat 32-bit data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000             ; a working 32-bit stack below 1MB

    call vga_print_pmode

    ; parse the loaded .xkex, place its sections at their physical addresses,
    ; and jump to the physical entry. The kernel's stub then sets up paging +
    ; the higher half. (Replaces the old flat copy-blob-and-jump.)
    call cxex_load_and_jump      ; no return on success

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

msg_pmode db 'CXK v5 stage 2 - 32-bit protected mode OK', 0

bits 16                          ; back to 16-bit for anything after this include