; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /boot/mem.asm  -  E820 memory map gathering (v5 stage 2)
; Aurora Tejeda / CATX Systems
;
; Gathers the BIOS memory map via int 15h / EAX=0xE820 straight into the CXBI
; memory map (cxbi.asm): an E820 entry - base(8) length(8) type(4) attrs(4) -
; is a struct cxk_mmap_entry already, and types 1-5 are CXBI's. This MUST run
; in real mode (BIOS is unavailable once we enter protected mode), so stage 2
; calls it BEFORE the pmode switch; cxbi_finish later turns the types CXBI
; does not have into reserved.
;
; We store the RAW map only. Filtering (usable vs reserved), finding the largest
; region, etc. is policy the kernel's memory manager does later in C.

E820_BUFFER        equ CXBI_MMAP     ; the entries, in the CXBI map
E820_ENTRY_SIZE    equ 24
E820_MAX_ENTRIES   equ 64
E820_SMAP          equ 0x534D4150    ; 'SMAP'

; gather_memory_map - fills E820_BUFFER and the CXBI entry count. Real mode.
gather_memory_map:
    push es
    xor ax, ax
    mov es, ax
    mov di, E820_BUFFER
    xor ebx, ebx                 ; continuation value = 0 to start
    xor bp, bp                   ; entry counter

.next:
    mov eax, 0xE820
    mov edx, E820_SMAP
    mov ecx, 24
    mov dword [es:di + 20], 1    ; ask for the ACPI 3.0 extended attr dword
    int 0x15
    jc .done                     ; CF set = end (or unsupported) 
    cmp eax, E820_SMAP
    jne .done                    ; EAX should read back 'SMAP'

    cmp ecx, 20
    jb .skip_entry               ; too-small entry, ignore but continue

    inc bp
    add di, E820_ENTRY_SIZE
    cmp bp, E820_MAX_ENTRIES
    jae .done

.skip_entry:
    test ebx, ebx                ; continuation 0 = that was the last entry
    jz .done
    jmp .next

.done:
    mov [CXBI_ADDR + CXBI_O_MMAP_COUNT], bp
    pop es
    ret