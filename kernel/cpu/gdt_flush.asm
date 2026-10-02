; /kernel/cpu/gdt_flush.asm
; Aurora Tejeda / CATX Systems
; Load the kernel GDT + reload segment registers, and load the TSS.

bits 32
global gdt_flush
global tss_flush

; void gdt_flush(uint32_t gdtr_addr)
;   loads the new GDT, then reloads DS/ES/FS/GS/SS to the ring-0 data selector
;   and far-jumps to reload CS with the ring-0 code selector.
gdt_flush:
    mov eax, [esp + 4]      ; arg: &gdtr
    lgdt [eax]

    mov ax, 0x10            ; KERNEL_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.reload_cs     ; KERNEL_CODE_SEL : far jump reloads CS
.reload_cs:
    ret

; void tss_flush(uint32_t tss_selector)
tss_flush:
    mov eax, [esp + 4]      ; arg: TSS selector (0x28)
    ltr ax
    ret