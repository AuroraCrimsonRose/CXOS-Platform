; /CXLite/kernel/kernel.asm
; Aurora Tejeda

; ASM entry point — bridges protected mode handoff to C kmain()
bits 32
global kernel_entry

extern kmain
extern __bss_start
extern __bss_end

section .text
kernel_entry:
    mov esp, 0x90000

    ; Zero the BSS. C guarantees that uninitialized statics/globals start at 0,
    ; but that's the runtime's job - the loader only copies the on-disk image
    ; (.text/.data), NOT the BSS (which has no disk contents). Without this,
    ; every static (hist_next, cur_line, the console history, CXFS buffers...)
    ; starts with whatever garbage was in RAM at boot - which differs per
    ; machine/emulator, causing baffling layout-dependent crashes.
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi              ; ecx = BSS size in bytes
    xor eax, eax              ; fill value = 0
    cld
    rep stosb                 ; zero [edi..edi+ecx)

    call kmain

.hang:
    cli
    hlt
    jmp .hang