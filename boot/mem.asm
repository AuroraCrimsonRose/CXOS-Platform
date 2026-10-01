; /boot/mem.asm  -  E820 memory map gathering (v5 stage 2)
; Aurora Tejeda / CATX SYSTEMS LLC
;
; Gathers the BIOS memory map via int 15h / EAX=0xE820 and stores the raw
; entries at a fixed buffer the kernel can read after handoff. This MUST run in
; real mode (BIOS is unavailable once we enter protected mode), so stage 2 calls
; it BEFORE the pmode switch.
;
; We store the RAW map only. Filtering (usable vs reserved), finding the largest
; region, etc. is policy the kernel's memory manager does later in C from this
; raw data - stage 2's job is just to capture it before BIOS access is lost.
;
; Layout the kernel will read:
;   E820_COUNT  (word) at E820_COUNT_ADDR : number of entries gathered
;   E820_BUFFER (array) at E820_BUFFER     : that many 24-byte entries
; Each entry: base(8) length(8) type(4) acpi_attrs(4) = 24 bytes.

; Layout the kernel will read (in the free 0x500 conventional-memory area,
; safely below stage 1 at 0x7C00 - NOT overlapping stage 2 at 0x7E00):
;   E820_COUNT  (word) at 0x0500 : number of entries gathered
;   E820_BUFFER (array) at 0x0504 : that many 24-byte entries
; Each entry: base(8) length(8) type(4) acpi_attrs(4) = 24 bytes.
; 64 entries * 24 = 1536 bytes -> 0x0504..0x0B04, clear of stage 1 at 0x7C00.

E820_COUNT_ADDR    equ 0x0500        ; entry count (word)
E820_BUFFER        equ 0x0504        ; raw entries
E820_ENTRY_SIZE    equ 24
E820_MAX_ENTRIES   equ 64
E820_SMAP          equ 0x534D4150    ; 'SMAP'

; gather_memory_map - fills E820_BUFFER, sets [E820_COUNT_ADDR]. Real mode.
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
    mov [E820_COUNT_ADDR], bp
    pop es
    ret