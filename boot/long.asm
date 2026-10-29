; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /boot/long.asm  -  stage 2: load the x86-64 kernel and enter long mode
; Aurora Tejeda / CATX Systems
;
; pmode.asm comes here once stage 2 is in 32-bit protected mode. Haiku's BIOS loader does the same from 32-bit protected mode
; (src/system/boot/platform/bios_ia32/long.cpp, long_start_kernel): check the
; CPU, place the ELF64 image, build page tables, set EFER.LME, turn paging on
; and jump through a 64-bit code segment.
;
; The image (docs/kernel/CX_KERNEL_PLAN.md section 2.4): every PT_LOAD segment
; is linked at KERNEL_VBASE + its physical address, KERNEL_VBASE being
; 0xFFFFFFFF80000000, the top 2 GiB. The loader maps the first GiB of physical
; memory twice with 2 MiB pages: at 0 (so this code survives turning paging on)
; and at KERNEL_VBASE (where the kernel runs). Those tables are the loader's,
; at 0x3000-0x6FFF; the kernel replaces them with its own (roadmap step 5c).
; EFER.NXE is set here, so the kernel's first tables can use NX.
;
; The kernel is entered in 64-bit mode at e_entry with RDI (and RBX) holding the physical address of the CXBI handover,
; RSP at 0x9F000, interrupts off. Anything this loader cannot place is
; refused with a message on screen and on COM1, never jumped to.

KERNEL_SRC          equ 0x10000         ; where load_kernel placed the image (stage2.asm)
ELF_MAGIC           equ 0x464C457F      ; 0x7F 'E' 'L' 'F' as a LE dword
ELF_BUF_SIZE        equ KERNEL_SECTORS * 512   ; what load_kernel read (stage2.asm)

; ELF64 header offsets
E_CLASS             equ 4
E_DATA              equ 5
E_TYPE              equ 16
E_MACHINE           equ 18
E_ENTRY             equ 24
E_PHOFF             equ 32
E_PHENTSIZE         equ 54
E_PHNUM             equ 56

; ELF64 program header offsets
P_TYPE              equ 0
P_OFFSET            equ 8
P_VADDR             equ 16
P_PADDR             equ 24
P_FILESZ            equ 32
P_MEMSZ             equ 40
PHDR_SIZE           equ 56

PT_LOAD             equ 1
ET_EXEC             equ 2
EM_X86_64           equ 0x3E

KERNEL_VBASE_HI     equ 0xFFFFFFFF      ; KERNEL_VBASE = 0xFFFFFFFF_80000000
KERNEL_VBASE_LO     equ 0x80000000
KERNEL_PHYS_MIN     equ 0x100000        ; nothing below 1 MiB: the loader lives there
LOW_MAP_END         equ 0x40000000      ; the 1 GiB both maps cover

PT_PML4             equ 0x3000
PT_PDPT_LO          equ 0x4000          ; PML4[0]   -> the identity map
PT_PDPT_HI          equ 0x5000          ; PML4[511] -> the top 512 GiB
PT_PD               equ 0x6000          ; 512 x 2 MiB, shared by both
PTE_P_RW            equ 0x003
PDE_P_RW_PS         equ 0x083

MSR_EFER            equ 0xC0000080
EFER_LME            equ 1 << 8
EFER_NXE            equ 1 << 11

COM1                equ 0x3F8

bits 32
; elf64_load_and_jump - the ELF at KERNEL_SRC is the x86-64 kernel. No return.
elf64_load_and_jump:
    call long_serial_init

    ; ---- the CPU: long mode and NX, or nothing (CX_BOOT.md section 3 item 2)
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29                   ; LM
    jz .no_long
    test edx, 1 << 20                   ; NX
    jz .no_nx

    ; ---- the header
    mov esi, KERNEL_SRC
    cmp dword [esi], ELF_MAGIC
    jne .bad_elf
    cmp byte [esi + E_CLASS], 2         ; ELFCLASS64
    jne .bad_elf
    cmp byte [esi + E_DATA], 1          ; little-endian
    jne .bad_elf
    cmp word [esi + E_TYPE], ET_EXEC
    jne .bad_elf
    cmp word [esi + E_MACHINE], EM_X86_64
    jne .bad_elf
    cmp word [esi + E_PHENTSIZE], PHDR_SIZE
    jne .bad_elf
    cmp dword [esi + E_ENTRY + 4], KERNEL_VBASE_HI
    jne .bad_elf
    cmp dword [esi + E_ENTRY], KERNEL_VBASE_LO + KERNEL_PHYS_MIN
    jb .bad_elf
    cmp dword [esi + E_ENTRY], KERNEL_VBASE_LO + LOW_MAP_END
    jae .bad_elf
    cmp dword [esi + E_PHOFF + 4], 0
    jne .bad_elf

    ; the program header table lies inside what was read
    movzx ecx, word [esi + E_PHNUM]
    mov eax, ecx
    imul eax, eax, PHDR_SIZE
    add eax, [esi + E_PHOFF]
    jc .bad_elf
    cmp eax, ELF_BUF_SIZE
    ja .bad_elf
    mov edi, esi
    add edi, [esi + E_PHOFF]            ; edi -> first program header
    mov dword [elf_loads], 0

.ph_loop:
    test ecx, ecx
    jz .placed
    cmp dword [edi + P_TYPE], PT_LOAD
    jne .ph_next

    ; every field this code reads as 32 bits really is
    cmp dword [edi + P_OFFSET + 4], 0
    jne .bad_seg
    cmp dword [edi + P_PADDR + 4], 0
    jne .bad_seg
    cmp dword [edi + P_FILESZ + 4], 0
    jne .bad_seg
    cmp dword [edi + P_MEMSZ + 4], 0
    jne .bad_seg

    ; KERNEL_PHYS_MIN <= paddr, paddr + memsz <= LOW_MAP_END
    mov eax, [edi + P_PADDR]
    cmp eax, KERNEL_PHYS_MIN
    jb .bad_seg
    mov edx, eax
    add edx, [edi + P_MEMSZ]
    jc .bad_seg
    cmp edx, LOW_MAP_END
    ja .bad_seg

    ; vaddr == KERNEL_VBASE + paddr: the only map the loader builds
    cmp dword [edi + P_VADDR + 4], KERNEL_VBASE_HI
    jne .bad_seg
    mov edx, eax
    add edx, KERNEL_VBASE_LO
    cmp [edi + P_VADDR], edx
    jne .bad_seg

    ; filesz <= memsz, and the file bytes lie inside what was read
    mov edx, [edi + P_FILESZ]
    cmp edx, [edi + P_MEMSZ]
    ja .bad_seg
    add edx, [edi + P_OFFSET]
    jc .bad_seg
    cmp edx, ELF_BUF_SIZE
    ja .bad_seg

    ; copy the file bytes to paddr, zero the rest up to memsz
    push ecx
    push edi
    mov ebx, edi
    mov esi, KERNEL_SRC
    add esi, [ebx + P_OFFSET]
    mov edi, eax
    mov ecx, [ebx + P_FILESZ]
    cld
    rep movsb
    mov ecx, [ebx + P_MEMSZ]
    sub ecx, [ebx + P_FILESZ]
    xor eax, eax
    rep stosb
    pop edi
    pop ecx
    inc dword [elf_loads]

.ph_next:
    add edi, PHDR_SIZE
    dec ecx
    jmp .ph_loop

.placed:
    cmp dword [elf_loads], 0
    je .bad_elf

    ; ---- the page tables: four zeroed pages, then the links
    mov edi, PT_PML4
    mov ecx, 4 * 4096 / 4
    xor eax, eax
    cld
    rep stosd
    mov dword [PT_PML4], PT_PDPT_LO | PTE_P_RW
    mov dword [PT_PML4 + 511 * 8], PT_PDPT_HI | PTE_P_RW
    mov dword [PT_PDPT_LO], PT_PD | PTE_P_RW
    mov dword [PT_PDPT_HI + 510 * 8], PT_PD | PTE_P_RW   ; 0xFFFFFFFF_80000000
    mov edi, PT_PD
    mov eax, PDE_P_RW_PS
    mov ecx, 512
.pd_loop:
    mov [edi], eax                      ; the high dword stays zero
    add eax, 0x200000
    add edi, 8
    loop .pd_loop

    mov esi, msg_long_enter
    call long_serial_puts

    ; ---- into long mode: PAE, CR3, EFER.LME and NXE, then paging
    mov eax, cr4
    or eax, 1 << 5                      ; CR4.PAE
    mov cr4, eax
    mov eax, PT_PML4
    mov cr3, eax
    mov ecx, MSR_EFER
    rdmsr
    or eax, EFER_LME | EFER_NXE
    wrmsr
    mov eax, cr0
    or eax, 1 << 31                     ; CR0.PG: long mode is now active,
    mov cr0, eax                        ; running 32-bit compatibility code
    jmp GDT_CODE64:long_entry

.no_long:
    mov esi, msg_no_long
    jmp long_refuse
.no_nx:
    mov esi, msg_no_nx
    jmp long_refuse
.bad_elf:
    mov esi, msg_bad_elf
    jmp long_refuse
.bad_seg:
    mov esi, msg_bad_seg
    jmp long_refuse

bits 64
long_entry:
    mov ax, GDT_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x9F000
    mov rax, [abs KERNEL_SRC + E_ENTRY]
    mov edi, CXBI_ADDR                  ; zero-extends: RDI = the handover
    mov ebx, CXBI_ADDR
    jmp rax                             ; into the kernel; no return

bits 32
; long_refuse - esi = message. Shows it on screen (row 12, red) and on COM1,
; then halts for good.
long_refuse:
    push esi
    call long_serial_puts
    pop esi
    mov edi, 0xB8000 + (12 * 80 * 2)
.vga:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0C                        ; bright red on black
    stosw
    jmp .vga
.halt:
    cli
    hlt
    jmp .halt

; long_serial_init - COM1 at 115200 8N1, as kernel/drivers/char/serial.c sets it.
long_serial_init:
    mov dx, COM1 + 1
    xor al, al
    out dx, al                          ; no interrupts
    mov dx, COM1 + 3
    mov al, 0x80
    out dx, al                          ; divisor latch
    mov dx, COM1
    mov al, 1
    out dx, al                          ; divisor 1: 115200
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3
    mov al, 0x03
    out dx, al                          ; 8N1
    mov dx, COM1 + 2
    mov al, 0xC7
    out dx, al                          ; FIFO on, cleared
    mov dx, COM1 + 4
    mov al, 0x03
    out dx, al                          ; DTR, RTS
    ret

; long_serial_puts - esi = NUL-terminated string; a CR LF follows it. With no
; UART fitted the status port reads 0xFF, so the wait never spins.
long_serial_puts:
.next:
    lodsb
    test al, al
    jz .eol
    call long_serial_putc
    jmp .next
.eol:
    mov al, 13
    call long_serial_putc
    mov al, 10
    call long_serial_putc
    ret

long_serial_putc:
    mov ah, al
    mov dx, COM1 + 5
.wait:
    in al, dx
    test al, 0x20                       ; THR empty
    jz .wait
    mov dx, COM1
    mov al, ah
    out dx, al
    ret

elf_loads       dd 0
msg_long_enter  db '[BOOT] x86-64 kernel placed; entering long mode', 0
msg_no_long     db 'CXK: this CPU has no long mode, so it cannot run CXOS', 0
msg_no_nx       db 'CXK: this CPU has no NX (no-execute), so it cannot run CXOS', 0
msg_bad_elf     db 'CXK: the kernel is not an x86-64 ELF image this loader can start', 0
msg_bad_seg     db 'CXK: a kernel segment lies outside what the loader maps', 0

bits 16                          ; back to 16-bit for anything after this include
