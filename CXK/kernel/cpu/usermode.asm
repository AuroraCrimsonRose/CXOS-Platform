; /CXK/kernel/cpu/usermode.asm
; Aurora Tejeda / CATX Systems LLC
; Ring 3 entry + syscall stub + return-to-kernel path.

bits 32

global enter_usermode
global syscall_stub
global return_to_kernel
extern syscall_dispatch     ; C: int syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2)

; ---------------------------------------------------------------------------
; int enter_usermode(uint32_t entry_eip, uint32_t user_esp)
;   Saves the kernel context, builds an iret frame, and drops to ring 3.
;   Returns ONLY when user code invokes SYS_EXIT, which routes through
;   return_to_kernel and makes this function return the exit value.
; ---------------------------------------------------------------------------
enter_usermode:
    push ebp
    push ebx
    push esi
    push edi                 ; save callee-saved regs (cdecl)
    pushfd                   ; save the kernel's EFLAGS (esp. the IF state)
    pop eax
    mov [saved_kernel_flags], eax
    ; save the kernel ESP at this point so return_to_kernel can restore it
    mov [saved_kernel_esp], esp

    mov eax, [esp + 20]      ; entry_eip  (4 saved regs*4 + ret(4) + arg1)
    mov ecx, [esp + 24]      ; user_esp

    cli
    mov bx, 0x23             ; USER_DATA_SEL
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx

    push 0x23                ; user SS
    push ecx                 ; user ESP
    pushfd
    pop edx
    or  edx, 0x200           ; IF = 1
    push edx                 ; EFLAGS
    push 0x1B                ; user CS
    push eax                 ; entry EIP
    iretd                    ; -> ring 3

; return_to_kernel(int retval): restore kernel stack + callee regs, return.
return_to_kernel:
    mov eax, [esp + 4]       ; retval
    mov esp, [saved_kernel_esp]
    mov bx, 0x10             ; kernel data
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    pop edi
    pop esi
    pop ebx
    pop ebp
    ; restore the kernel's original EFLAGS (re-enables interrupts if the kernel
    ; had them on before entering usermode; the syscall path left IF=0, which
    ; would otherwise deadlock the kernel on its next hlt).
    push dword [saved_kernel_flags]
    popfd
    ret                      ; returns retval (in eax) from enter_usermode

; ---------------------------------------------------------------------------
; syscall_stub - the int 0x80 handler.
; ---------------------------------------------------------------------------
syscall_stub:
    cli
    pushad
    mov bp, 0x10             ; kernel data segments
    mov ds, bp
    mov es, bp
    mov fs, bp
    mov gs, bp

    push ecx                 ; arg2
    push ebx                 ; arg1
    push eax                 ; syscall number
    call syscall_dispatch
    add esp, 12

    ; store return value into saved eax (pushad: eax at [esp+28])
    mov [esp + 28], eax

    popad
    sti
    iretd

section .bss
saved_kernel_esp:   resd 1
saved_kernel_flags: resd 1

; ---------------------------------------------------------------------------
; A tiny, position-independent ring-3 routine (only int 0x80, no relative
; calls/jumps that would break when copied to the user page). It expects the
; address of its message string passed in via the user stack is NOT used;
; instead the kernel patches the message pointer into ebx by convention:
; we load the string address from a fixed location the kernel sets up.
;
; To keep it truly position-independent and simple, the kernel passes the
; message pointer in the user stack and this routine reads it. But simplest of
; all: the kernel writes the msg pointer into a known user page slot. Here we
; take the message pointer in the initial eax/ebx set by the kernel before
; entry is not possible (iret clears regs path), so we embed the syscall
; sequence and read the pointer from [user_esp] which the kernel pre-seeds.
; ---------------------------------------------------------------------------
section .text
global user_blob_start
global user_blob_end
user_blob_start:
    ; ebx = message pointer was placed at [esp] by the kernel (top of user stack)
    mov ebx, [esp]
    mov eax, 1          ; SYS_WRITE
    int 0x80
    mov eax, 0          ; SYS_EXIT
    xor ebx, ebx
    int 0x80
.hang:
    jmp .hang
user_blob_end: