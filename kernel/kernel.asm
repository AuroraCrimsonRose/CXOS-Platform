; /kernel/kernel.asm - v5 higher-half entry stub
; Aurora Tejeda / CATX Systems
;
; Stage 2 jumps here at the PHYSICAL entry (0x100000) with paging OFF, in flat
; 32-bit protected mode. This stub performs the higher-half bootstrap:
;   1. Build a page directory + one page table mapping the first 4 MB.
;   2. Install that mapping at BOTH the identity slot (so the next instruction
;      after enabling paging, still at a physical EIP, doesn't fault) AND the
;      higher-half slot (so 0xC0000000+ maps to physical 0).
;   3. Enable paging.
;   4. Jump to a VIRTUAL (higher-half) label - now EIP is high.
;   5. Drop the identity map (no longer needed), then prove we're high by
;      writing to VGA via the higher-half-mapped address.
;
; Increment 1 goal: prove the kernel executes in the higher half. No C / kmain
; yet - that layers on once this transition is verified.
;
; Addressing rule for the pre-paging part: all symbols resolve to VIRTUAL
; addresses (linker links us high), so to use them as PHYSICAL addresses before
; paging is on we subtract KERNEL_VBASE. The PHYS() macro does that.

KERNEL_VBASE equ 0xC0000000

%define PHYS(x) ((x) - KERNEL_VBASE)

; PDE/PTE flag bits
PG_PRESENT  equ 0x1
PG_WRITE    equ 0x2
PG_PS_4M    equ 0x80          ; (not used; we use 4 KB pages via a page table)

bits 32
global kernel_entry
extern __kernel_vbase
extern kmain
extern __bss_start
extern __bss_end

section .text
kernel_entry:
    ; --- running at PHYSICAL ~0x100000, paging OFF ---
    cli

    ; Build a page table that maps the first 4 MB (1024 * 4 KB) identity-style:
    ; PTE[i] -> physical (i * 0x1000), present+write. This covers the kernel
    ; (at 1 MB) and low memory / VGA (0xB8000).
    mov edi, PHYS(boot_page_table)
    xor eax, eax                     ; phys addr 0, += 0x1000 each entry
    mov ecx, 1024                    ; 1024 entries = 4 MB
    or  eax, PG_PRESENT | PG_WRITE
.fill_pt:
    mov [edi], eax
    add eax, 0x1000
    add edi, 4
    loop .fill_pt

    ; Page directory: point BOTH the identity slot (index 0, covers
    ; 0x00000000-0x003FFFFF) and the higher-half slot (index 0xC0000000>>22 =
    ; 768, covers 0xC0000000-0xC03FFFFF) at the same page table.
    mov edi, PHYS(boot_page_dir)
    ; zero the whole directory first
    push edi
    xor eax, eax
    mov ecx, 1024
.zero_pd:
    mov [edi], eax
    add edi, 4
    loop .zero_pd
    pop edi

    mov eax, PHYS(boot_page_table)
    or  eax, PG_PRESENT | PG_WRITE
    mov [edi + 0 * 4], eax           ; PDE[0]   identity map (low 4 MB)
    mov [edi + 768 * 4], eax         ; PDE[768] higher-half (0xC0000000)

    ; Load CR3 with the physical address of the page directory.
    mov eax, PHYS(boot_page_dir)
    mov cr3, eax

    ; Enable paging (CR0.PG = bit 31).
    mov eax, cr0
    or  eax, 0x80000000
    mov cr0, eax

    ; We're now paged, but EIP is still low (identity map keeps us alive).
    ; Jump to a VIRTUAL label to start executing in the higher half.
    lea eax, [higher_half_entry]     ; this resolves to a 0xC01xxxxx address
    jmp eax

higher_half_entry:
    ; --- now executing at a HIGHER-HALF virtual address ---
    ; Drop the identity map; we don't need low addresses mapped anymore.
    mov dword [boot_page_dir + 0 * 4], 0
    mov eax, cr3
    mov cr3, eax                     ; reload CR3 to flush the TLB

    ; Set up a real stack at a HIGHER-HALF address. The stack stage 2 left us on
    ; (~0x90000) is LOW physical memory no longer mapped now that the identity
    ; map is gone - using it would fault on the first push/call. Point esp at the
    ; top of a stack reserved in .bss (which IS mapped high).
    mov esp, kernel_stack_top

    ; Zero the BSS. C assumes uninitialized statics start at 0, but the loader
    ; only copies .text/.data from disk - .bss has no disk image. Done after esp
    ; is set but before any push/call, so zeroing the stack region is harmless.
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi                     ; ecx = BSS size in bytes
    xor eax, eax
    cld
    rep stosb

    ; Prove we're in the higher half: write a green banner to VGA. VGA is at
    ; physical 0xB8000, which our page table maps at virtual
    ; 0xC0000000 + 0xB8000 (via PDE[768]). Write there.
    mov edi, 0xC0000000 + 0xB8000 + (12 * 80 * 2)   ; row 12
    mov esi, msg_hh
.print:
    mov al, [esi]
    test al, al
    jz .done
    mov ah, 0x0A                     ; bright green
    mov [edi], ax
    add edi, 2
    inc esi
    jmp .print
.done:

    ; into C - kmain runs in the higher half on the .bss stack.
    call kmain

.hang:
    cli
    hlt
    jmp .hang

section .rodata
msg_hh db 'CXK kernel - running in the higher half (0xC0100000)', 0

; Page structures live in their OWN section (.pagetables), NOT .bss. This is
; deliberate: the BSS-zeroing loop runs AFTER paging is enabled (CR3 points at
; boot_page_dir). If the page dir/table were in .bss, zeroing them would wipe
; the active page directory and instantly triple-fault on the next page walk.
; Keeping them out of the __bss_start..__bss_end range avoids that.
section .pagetables nobits align=4096
global boot_page_dir
boot_page_dir:   resb 4096
global boot_page_table
boot_page_table: resb 4096

; kernel stack (higher-half). In .bss is fine - it's zeroed before any push,
; and esp isn't used until the call to kmain after zeroing. 16 KB, grows down.
section .bss
align 16
kernel_stack_bottom:
    resb 16384
kernel_stack_top: