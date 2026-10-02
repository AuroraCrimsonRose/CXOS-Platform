; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /CXLite/boot/a20.asm
; Aurora Tejeda

; enable_a20
; tries BIOS, fast A20, then kbd
enable_a20:
    call check_a20
    jc .done

    mov ax, 0x2401
    int 0x15
    call check_a20
    jc .done

    in al, 0x92
    test al, 0x02
    jnz .done
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    call check_a20
    jc .done

    call enable_a20_kbd
    call check_a20
    jc .done

    mov si, msg_a20_fail
    call print_string
    jmp halt

.done:
    mov si, msg_a20_ok
    call print_string
    call print_newline
    ret

; enable_a20_kbd
; 8042 keyboard controller method
enable_a20_kbd:
    call .wait_input
    mov al, 0xAD
    out 0x64, al

    call .wait_input
    mov al, 0xD0
    out 0x64, al

    call .wait_output
    in al, 0x60
    push ax

    call .wait_input
    mov al, 0xD1
    out 0x64, al

    call .wait_input
    pop ax
    or al, 0x02
    out 0x60, al

    call .wait_input
    mov al, 0xAE
    out 0x64, al

    call .wait_input
    ret

.wait_input:
    in al, 0x64
    test al, 0x02
    jnz .wait_input
    ret

.wait_output:
    in al, 0x64
    test al, 0x01
    jz .wait_output
    ret

; check_a20
; carry set = enabled
; carry clear = disabled
check_a20:
    push ds
    push es
    push di
    push si

    xor ax, ax
    mov ds, ax
    mov si, 0x0500

    mov ax, 0xFFFF
    mov es, ax
    mov di, 0x0510

    mov al, [ds:si]
    push ax
    mov al, [es:di]
    push ax

    mov byte [ds:si], 0x00
    mov byte [es:di], 0xFF

    cmp byte [ds:si], 0xFF
    je .disabled

    pop ax
    mov [es:di], al
    pop ax
    mov [ds:si], al

    pop si
    pop di
    pop es
    pop ds
    stc
    ret

.disabled:
    pop ax
    mov [es:di], al
    pop ax
    mov [ds:si], al

    pop si
    pop di
    pop es
    pop ds
    clc
    ret