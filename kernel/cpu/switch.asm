; /kernel/cpu/switch.asm
; Aurora Tejeda / CATX Systems LLC
; Cooperative kernel-thread context switch.

bits 32
global context_switch

; void context_switch(uint32_t *old_esp, uint32_t new_esp)
;   Saves the current thread's callee-saved registers on its stack, stores the
;   resulting ESP into *old_esp, then loads new_esp and restores the next
;   thread's saved registers. Returns into the next thread.
;
;   For COOPERATIVE switching we only need to preserve the callee-saved regs
;   (ebx, esi, edi, ebp) per the cdecl convention - the caller already saved
;   eax/ecx/edx. EFLAGS is preserved too so a switched-out thread resumes with
;   its own flag state.
context_switch:
    mov eax, [esp + 4]      ; old_esp (pointer to where we save the old ESP)
    mov edx, [esp + 8]      ; new_esp (the next thread's saved ESP)

    ; save callee-saved registers + flags on the CURRENT stack
    pushfd
    push ebx
    push esi
    push edi
    push ebp

    ; save the current ESP into *old_esp
    mov [eax], esp

    ; load the next thread's stack
    mov esp, edx

    ; restore its saved registers (mirror order)
    pop ebp
    pop edi
    pop esi
    pop ebx
    popfd

    ret                     ; return into the next thread