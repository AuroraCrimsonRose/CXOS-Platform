; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /CXLite/boot/print.asm
; Aurora Tejeda

; print_clr
; clears the screen via VGA
print_clr:
    mov ax, VGA_SEGMENT
    mov es, ax
    xor di, di
    mov cx, 2000
    mov ax, 0x0720
    rep stosw
    mov ah, 0x02
    mov bh, 0x00
    mov dh, 0x00
    mov dl, 0x00
    int 0x10
    ret

; print_char
; in: AL = character
print_char:
    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10
    ret

; print_string
; in: DS:SI -> null terminated string
print_string:
    push ax
    .loop:
        lodsb
        test al, al
        jz .done
        call print_char
        jmp .loop
    .done:
    pop ax
    ret

; print_newline
print_newline:
    push ax
    mov al, 0x0D
    call print_char
    mov al, 0x0A
    call print_char
    pop ax
    ret

; print_hex32
; in: EAX = 32 bit value
print_hex32:
    push eax
    push ebx
    push ecx
    mov cx, 8
    .loop:
        rol eax, 4
        mov bl, al
        and bl, 0x0F
        cmp bl, 9
        jbe .is_digit
        add bl, 'A' - 10
        jmp .emit
    .is_digit:
        add bl, '0'
    .emit:
        mov al, bl
        call print_char
        loop .loop
    pop ecx
    pop ebx
    pop eax
    ret

; print_hex64
; in: EBX = high dword, EAX = low dword
print_hex64:
    push eax
    mov eax, ebx
    call print_hex32
    pop eax
    call print_hex32
    ret