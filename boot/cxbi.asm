; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /boot/cxbi.asm  -  the CXBI handover, filled by stage 2
; Aurora Tejeda / CATX Systems
;
; Stage 2 hands the kernel one structure, struct cxk_boot_info (abi/cxk_boot.h),
; and passes its physical address in EBX; the kernel reads nothing else the
; loader left (docs/boot/CX_BOOT.md section 3.3, roadmap step 4). Real-mode
; code fills what only the BIOS can give - the memory map (mem.asm), the
; framebuffer (vbe.asm) - straight into it; cxbi_finish, in protected mode,
; adds the rest and seals it with the magic. cxexload.asm puts its address in EBX
; as it jumps to the kernel.
;
; The addresses stage 2 used before CXBI, 0x500 (the E820 map) and 0x1C40 (the
; VBE mode), are filled with CXBI_POISON, so a kernel that still read them
; would get nonsense rather than a map that happens to be right.
;
; Offsets mirror abi/cxk_boot.h; the kernel checks magic, version and size.

CXBI_ADDR          equ 0x2000      ; the structure (128 bytes)
CXBI_MMAP          equ 0x2100      ; its memory map, up to E820_MAX_ENTRIES
CXBI_AREA_END      equ 0x2800

CXBI_O_MAGIC       equ 0
CXBI_O_VERSION     equ 4           ; word
CXBI_O_SIZE        equ 6           ; word
CXBI_O_MMAP_ADDR   equ 8
CXBI_O_FB_ADDR     equ 16
CXBI_O_FB_SIZE     equ 24
CXBI_O_ACPI_RSDP   equ 32
CXBI_O_FIRMWARE    equ 72
CXBI_O_MMAP_COUNT  equ 80
CXBI_O_MMAP_ESIZE  equ 84
CXBI_O_FB_WIDTH    equ 88
CXBI_O_FB_HEIGHT   equ 92
CXBI_O_FB_PITCH    equ 96
CXBI_O_CPUID_MAX   equ 100
CXBI_O_CPUID1_ECX  equ 104
CXBI_O_CPUID1_EDX  equ 108
CXBI_O_FB_BPP      equ 112         ; bytes from here
CXBI_O_FB_RBITS    equ 113
CXBI_O_FB_GBITS    equ 114
CXBI_O_FB_BBITS    equ 115
CXBI_O_FB_RSHIFT   equ 116
CXBI_O_FB_GSHIFT   equ 117
CXBI_O_FB_BSHIFT   equ 118
CXBI_O_BOOT_DISK   equ 120         ; qword: the XBPT disk id (stage2 find_boot_partition)

CXBI_MAGIC_VAL     equ 0x49425843  ; "CXBI"
CXBI_VERSION_VAL   equ 2
CXBI_SIZE_VAL      equ 128
CXBI_ENTRY_SIZE    equ 24
CXBI_FW_BIOS       equ 1
CXBI_MEM_RESERVED  equ 2
CXBI_MEM_LAST_E820 equ 5           ; E820 types 1-5 are CXBI's 1-5

CXBI_POISON        equ 0xA5
OLD_E820_ADDR      equ 0x0500      ; count word, then entries
OLD_E820_END       equ 0x0B04
OLD_VBE_ADDR       equ 0x1C40
OLD_VBE_END        equ 0x1C50

bits 16
; cxbi_clear - zero the structure and its map. Real mode, before anything
; writes into them.
cxbi_clear:
    push es
    push di
    push cx
    push ax
    xor ax, ax
    mov es, ax
    mov di, CXBI_ADDR
    mov cx, CXBI_AREA_END - CXBI_ADDR
    cld
    rep stosb
    pop ax
    pop cx
    pop di
    pop es
    ret

bits 32
; cxbi_finish - fill the rest, seal it, poison the old addresses, and leave
; Protected mode, flat segments.
cxbi_finish:
    ; memory map: E820 types past 5 (persistent memory, vendor types) and the
    ; undefined 0 become reserved, so the kernel sees only CXBI's types
    mov ecx, [CXBI_ADDR + CXBI_O_MMAP_COUNT]
    mov esi, CXBI_MMAP
.type_loop:
    jecxz .types_done
    mov eax, [esi + 16]
    test eax, eax
    jz .type_res
    cmp eax, CXBI_MEM_LAST_E820
    jbe .type_ok
.type_res:
    mov dword [esi + 16], CXBI_MEM_RESERVED
.type_ok:
    add esi, CXBI_ENTRY_SIZE
    dec ecx
    jmp .type_loop
.types_done:
    mov dword [CXBI_ADDR + CXBI_O_MMAP_ADDR], CXBI_MMAP
    mov dword [CXBI_ADDR + CXBI_O_MMAP_ESIZE], CXBI_ENTRY_SIZE

    ; framebuffer size, if vbe.asm set one
    cmp dword [CXBI_ADDR + CXBI_O_FB_ADDR], 0
    je .no_fb
    mov eax, [CXBI_ADDR + CXBI_O_FB_PITCH]
    mul dword [CXBI_ADDR + CXBI_O_FB_HEIGHT]
    mov [CXBI_ADDR + CXBI_O_FB_SIZE], eax
    mov [CXBI_ADDR + CXBI_O_FB_SIZE + 4], edx
.no_fb:

    ; the ACPI RSDP: the first 1 KB of the EBDA, then the BIOS ROM area
    movzx esi, word [0x40E]
    shl esi, 4
    xor eax, eax
    cmp esi, 0x80000
    jb .rom
    cmp esi, 0xA0000
    jae .rom
    mov ecx, 1024 / 16
    call rsdp_scan
    test eax, eax
    jnz .rsdp_found
.rom:
    mov esi, 0xE0000
    mov ecx, 0x20000 / 16
    call rsdp_scan
.rsdp_found:
    mov [CXBI_ADDR + CXBI_O_ACPI_RSDP], eax

    ; CPUID as the loader saw it
    xor eax, eax
    cpuid
    mov [CXBI_ADDR + CXBI_O_CPUID_MAX], eax
    mov eax, 1
    cpuid
    mov [CXBI_ADDR + CXBI_O_CPUID1_ECX], ecx
    mov [CXBI_ADDR + CXBI_O_CPUID1_EDX], edx

    mov dword [CXBI_ADDR + CXBI_O_FIRMWARE], CXBI_FW_BIOS

    ; poison what the kernel used to read
    mov al, CXBI_POISON
    cld
    mov edi, OLD_E820_ADDR
    mov ecx, OLD_E820_END - OLD_E820_ADDR
    rep stosb
    mov edi, OLD_VBE_ADDR
    mov ecx, OLD_VBE_END - OLD_VBE_ADDR
    rep stosb

    ; sealed last: a structure without its magic is one the kernel refuses
    mov word [CXBI_ADDR + CXBI_O_VERSION], CXBI_VERSION_VAL
    mov word [CXBI_ADDR + CXBI_O_SIZE], CXBI_SIZE_VAL
    mov dword [CXBI_ADDR + CXBI_O_MAGIC], CXBI_MAGIC_VAL
    ret

; rsdp_scan - ESI = start, ECX = 16-byte steps. EAX = the RSDP's address (its
; signature and the checksum of its first 20 bytes both good), or 0.
rsdp_scan:
.next:
    cmp dword [esi], 'RSD '
    jne .skip
    cmp dword [esi + 4], 'PTR '
    jne .skip
    push ecx
    xor eax, eax
    xor edx, edx
    mov ecx, 20
.sum:
    add al, [esi + edx]
    inc edx
    loop .sum
    pop ecx
    test al, al
    jnz .skip
    mov eax, esi
    ret
.skip:
    add esi, 16
    loop .next
    xor eax, eax
    ret

bits 16
