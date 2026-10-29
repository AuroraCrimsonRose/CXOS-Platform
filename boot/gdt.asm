; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /CXLite/boot/gdt.asm
; Aurora Tejeda

; segment selectors
; these are the offsets into the GDT
; used to reload segment registers after pmode switch - this is basically magic number games basically leading into protected mode.
GDT_NULL        equ 0x00    ; null descriptor
GDT_CODE        equ 0x08    ; code segment
GDT_DATA        equ 0x10    ; data segment
GDT_CODE64      equ 0x18    ; 64-bit code segment (long.asm only)
GDT_DATA16      equ 0x20    ; 64 KB data segment (CXK_TEST_HOSTILE_BIOS only)

; GDT table
gdt_start:

    ; null descriptor — required by CPU
    ; all zeros, any access causes GPF
    gdt_null:
        dq 0x0000000000000000

    ; code segment descriptor
    ; base:  0x00000000
    ; limit: 0xFFFFF (4GB with G bit)
    ; ring:  0 (kernel)
    ; type:  execute / read
    gdt_code:
        dw 0xFFFF           ; limit low 16 bits
        dw 0x0000           ; base  low 16 bits
        db 0x00             ; base  mid  8 bits
        db 0x9A             ; access byte:
                            ;   P=1  present
                            ;   DPL=00 ring 0
                            ;   S=1  code/data
                            ;   E=1  executable
                            ;   DC=0 non conforming
                            ;   RW=1 readable
                            ;   A=0  not accessed
        db 0xCF             ; flags + limit high:
                            ;   G=1  4KB granularity
                            ;   DB=1 32-bit
                            ;   L=0  not 64-bit
                            ;   limit high = 0xF
        db 0x00             ; base high 8 bits

    ; data segment descriptor
    ; base:  0x00000000
    ; limit: 0xFFFFF (4GB with G bit)
    ; ring:  0 (kernel)
    ; type:  read / write
    gdt_data:
        dw 0xFFFF           ; limit low 16 bits
        dw 0x0000           ; base  low 16 bits
        db 0x00             ; base  mid  8 bits
        db 0x92             ; access byte:
                            ;   P=1  present
                            ;   DPL=00 ring 0
                            ;   S=1  code/data
                            ;   E=0  not executable
                            ;   DC=0 grows up
                            ;   RW=1 writable
                            ;   A=0  not accessed
        db 0xCF             ; flags + limit high:
                            ;   G=1  4KB granularity
                            ;   DB=1 32-bit
                            ;   L=0  not 64-bit
                            ;   limit high = 0xF
        db 0x00             ; base high 8 bits

    ; 64-bit code segment descriptor, for the jump into long mode (long.asm).
    ; Base and limit are ignored in 64-bit mode; what matters is L=1 with D=0.
    gdt_code64:
        dw 0xFFFF           ; limit low 16 bits
        dw 0x0000           ; base  low 16 bits
        db 0x00             ; base  mid  8 bits
        db 0x9A             ; access byte: present, ring 0, code, readable
        db 0xAF             ; flags + limit high:
                            ;   G=1  4KB granularity
                            ;   DB=0 (must be 0 when L=1)
                            ;   L=1  64-bit
                            ;   limit high = 0xF
        db 0x00             ; base high 8 bits

%if CXK_TEST_HOSTILE_BIOS
    ; 16-bit data segment with a 64 KB limit: what a hostile BIOS leaves in FS
    ; and GS after int 13h (stage2.asm hostile_bios_limits). Test builds only.
    gdt_data16:
        dw 0xFFFF           ; limit 64 KB
        dw 0x0000
        db 0x00
        db 0x92             ; present, ring 0, data, writable
        db 0x00             ; G=0 byte granularity, B=0
        db 0x00
%endif

gdt_end:

; loaded by lgdt instruction in pmode.asm
gdt_descriptor:
    dw gdt_end - gdt_start - 1  ; limit = size - 1
    dd gdt_start                ; linear base address