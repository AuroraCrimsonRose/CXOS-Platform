; /CXK/boot/cxexload.asm  -  stage 2 CXEX kernel loader (32-bit protected mode)
; Aurora Tejeda / CATX SYSTEMS LLC
;
; Replaces the old flat "copy blob + jump" with real CXEX loading: parse the
; .xkex header at the low load buffer, place each section at its PHYSICAL load
; address, then jump to the physical entry. The kernel's own stub then sets up
; paging + the higher-half map (unchanged).
;
; Physical placement: the kernel is linked higher-half (sections at virtual
; 0xC01xxxxx) but must be PLACED physically (paging is still off here). The
; CXEX header carries phys_base (the physical load base, from the ELF's LMA via
; mkcxes). The virt->phys delta = load_base(virtual) - phys_base. Each section's
; physical address = section.virt_addr - delta; physical entry = entry - delta.
; This makes the file self-describing - no hardcoded higher-half constant here.
;
; Runs in 32-bit pmode (flat segments already set). The .xkex sits at CXEX_SRC.

; ---- CXEX on-disk offsets (must match lib/cxex + mkcxes) ----
CXEX_SRC            equ 0x10000     ; where load_kernel placed the .xkex

; header field offsets
CXH_MAGIC           equ 0
CXH_ENTRY           equ 16
CXH_LOAD_BASE       equ 20
CXH_SEC_COUNT       equ 32
CXH_SEC_OFF         equ 34
CXH_PHYS_BASE       equ 48
; section entry (28 bytes): name(8) file_off(4) virt(12)... 
CXS_SIZE            equ 28
CXS_FILE_OFF        equ 8
CXS_VIRT            equ 12
CXS_FILE_SIZE       equ 16
CXS_MEM_SIZE        equ 20

bits 32
; cxex_load_and_jump - parse the .xkex at CXEX_SRC, place sections, jump to entry.
; No return on success. On bad magic, writes a marker and halts.
cxex_load_and_jump:
    mov esi, CXEX_SRC

    ; check magic 'CXEX' - bytes C,X,E,X = 0x43,0x58,0x45,0x58, which as a
    ; little-endian dword load is 0x58455843.
    cmp dword [esi + CXH_MAGIC], 0x58455843
    jne .badmagic

    ; delta = load_base(virtual) - phys_base
    mov eax, [esi + CXH_LOAD_BASE]
    sub eax, [esi + CXH_PHYS_BASE]
    mov [cxex_delta], eax

    ; loop over sections
    movzx ecx, word [esi + CXH_SEC_COUNT]      ; section count
    movzx edx, word [esi + CXH_SEC_OFF]        ; offset to section table
    lea edi, [esi + edx]                        ; edi -> first section entry

.sec_loop:
    test ecx, ecx
    jz .done_sections

    ; dest physical = section.virt_addr - delta
    mov eax, [edi + CXS_VIRT]
    sub eax, [cxex_delta]
    mov ebx, eax                                ; ebx = dest phys

    ; src = CXEX_SRC + section.file_off
    mov esi, CXEX_SRC
    add esi, [edi + CXS_FILE_OFF]

    ; copy file_size bytes (esi -> ebx)
    push ecx
    push edi
    mov ecx, [edi + CXS_FILE_SIZE]
    mov edi, ebx
    cld
    rep movsb                                   ; copy file bytes

    ; zero the rest up to mem_size (bss-style tail of the section)
    pop edi
    mov ecx, [edi + CXS_MEM_SIZE]
    sub ecx, [edi + CXS_FILE_SIZE]              ; bytes to zero
    jz .no_zero
    ; edi currently = dest end? no - recompute: dest + file_size
    mov eax, [edi + CXS_VIRT]
    sub eax, [cxex_delta]
    add eax, [edi + CXS_FILE_SIZE]
    push edi
    mov edi, eax
    xor al, al
    rep stosb
    pop edi
.no_zero:
    pop ecx

    add edi, CXS_SIZE                           ; next section entry
    dec ecx
    jmp .sec_loop

.done_sections:
    ; jump to physical entry = entry - delta
    mov esi, CXEX_SRC
    mov eax, [esi + CXH_ENTRY]
    sub eax, [cxex_delta]
    jmp eax                                      ; into the kernel (paging off)

.badmagic:
    ; write 'CX?' marker at row 0 col 0 and halt
    mov dword [0xB8000], 0x0F43                  ; 'C' white
    mov dword [0xB8000+2], 0x0F58                ; 'X'
    mov dword [0xB8000+4], 0x0F3F                ; '?'
.bmhang:
    hlt
    jmp .bmhang

cxex_delta dd 0