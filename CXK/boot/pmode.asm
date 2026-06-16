; /CXLite/boot/pmode.asm
; Aurora Tejeda

; no return — execution continues at pmode_entry
enter_protected_mode:
    cli
    xor ax, ax
    mov ds, ax
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 0x1
    mov cr0, eax

    jmp GDT_CODE:pmode_entry     ; nothing between cr0 and this

; pmode_entry
bits 32
pmode_entry:
    mov ax, GDT_DATA            ; 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    call vga_print_pmode

    jmp KERNEL_LOAD_ADDRESS     ; hand off to kernel — no return

; writes msg_pmode_ok to row 7
vga_print_pmode:
    push eax
    push edi
    push esi

    mov edi, 0xB8460

    ; Print base message in white
    mov esi, msg_pmode_base

    .base_loop:
        lodsb
        test al, al
        jz .print_ok
        mov ah, 0x07
        stosw
        jmp .base_loop

    .print_ok:
        mov esi, msg_pmode_ok

    .ok_loop:
        lodsb
        test al, al
        jz .done
        mov ah, 0x0A
        stosw
        jmp .ok_loop

    .done:
        pop esi
        pop edi
        pop eax
        ret

; pmode_halt
pmode_halt:
    hlt
    jmp pmode_halt

; strings
msg_pmode_base db "[BOOT] PMODE CHECK ", 0
msg_pmode_ok   db "OK", 0

; back to 16-bit for anything after this include
bits 16