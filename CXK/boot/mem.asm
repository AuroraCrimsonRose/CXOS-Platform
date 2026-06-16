; /CXLite/boot/mem.asm - i hate memory management maybe i should of just stuck to C++
; Aurora Tejeda

; stores entries at E820_BUFFER
init_memory:
    xor ebx, ebx                ; continuation = 0
    xor bp, bp                  ; entry count = 0
    xor edi, edi  
    mov di, E820_BUFFER
    xor ax, ax
    mov es, ax

    .next:
        mov eax, 0xE820
        mov edx, 0x534D4150     ; 'SMAP'

        mov dword [es:di + 20], 1
        mov ecx, 24

        push bp
        int 0x15
        pop bp
        jc .no_e820

        cmp eax, 0x534D4150
        jne .no_e820

        cmp ecx, 20
        jb .done

        add di, MEM_REGION_SIZE ; fixed 24 byte stride

        inc bp

        cmp bp, E820_MAX_ENTRIES
        jae .done

        test ebx, ebx
        jz .done
        jmp .next

    .no_e820:
        mov word [E820_ENTRY_COUNT], 0
        ret

    .done:
        mov word [E820_ENTRY_COUNT], bp
        ret

process_e820:
    cld
    xor esi, esi          ; clear upper 16 bits
    xor edi, edi
    mov si, E820_BUFFER
    mov di, E820_CLEAN_BUFFER

    xor cx, cx                  ; source entry index
    xor dx, dx                  ; clean entry count

    mov bx, [E820_ENTRY_COUNT]

    test bx, bx
    jz .done

    .loop:
        cmp cx, bx
        jae .done

        mov eax, [si + MEM_TYPE]
        cmp eax, E820_TYPE_USABLE
        jne .skip

        ; skip regions below 1MB
        mov eax, [si + MEM_LENGTH + 4]  ; length hi
        test eax, eax
        jnz .size_ok                    ; hi nonzero = definitely >= 1MB
        mov eax, [si + MEM_LENGTH]      ; length lo
        cmp eax, 0x100000
        jb .skip

    .size_ok:
        push cx
        push si
        xor ecx, ecx
        mov cx, 6               ; 24 bytes / 4 = 6 dwords
        rep movsd               ; di auto advances

        pop si
        pop cx

        inc dx

    .skip:
        add si, MEM_REGION_SIZE
        inc cx
        jmp .loop

    .done:
        mov word [E820_CLEAN_COUNT], dx
        ret

; scans E820_CLEAN_BUFFER for largest usable region
; stores result in KERNEL_RAM_BASE / KERNEL_RAM_SIZE
find_largest_region:
    xor esi, esi
    mov si, E820_CLEAN_BUFFER
    mov cx, [E820_CLEAN_COUNT]

    test cx, cx
    jz .no_memory

    mov dword [KERNEL_RAM_BASE_LO], 0
    mov dword [KERNEL_RAM_BASE_HI], 0
    mov dword [KERNEL_RAM_SIZE_LO], 0
    mov dword [KERNEL_RAM_SIZE_HI], 0

    .loop:
        test cx, cx
        jz .done

        ; compare length hi first
        mov eax, [si + MEM_LENGTH + 4]
        cmp eax, [KERNEL_RAM_SIZE_HI]
        ja  .new_largest
        jb  .next

        ; hi equal, compare length lo
        mov eax, [si + MEM_LENGTH]
        cmp eax, [KERNEL_RAM_SIZE_LO]
        jbe .next

    .new_largest:
        mov eax, [si + MEM_BASE]
        mov [KERNEL_RAM_BASE_LO], eax

        mov eax, [si + MEM_BASE + 4]
        mov [KERNEL_RAM_BASE_HI], eax

        mov eax, [si + MEM_LENGTH]
        mov [KERNEL_RAM_SIZE_LO], eax

        mov eax, [si + MEM_LENGTH + 4]
        mov [KERNEL_RAM_SIZE_HI], eax

    .next:
        add si, MEM_REGION_SIZE
        dec cx
        jmp .loop

    .done:
        ret

    .no_memory:
        mov si, msg_no_mem
        call panic

; prints kernel RAM base and size
debug_memory:
    push eax
    push ebx

    call print_newline

    mov si, msg_base
    call print_string
    mov ebx, [KERNEL_RAM_BASE_HI]
    mov eax, [KERNEL_RAM_BASE_LO]
    call print_hex64
    call print_newline

    mov si, msg_size
    call print_string
    mov ebx, [KERNEL_RAM_SIZE_HI]
    mov eax, [KERNEL_RAM_SIZE_LO]
    call print_hex64
    call print_newline

    pop ebx
    pop eax
    ret