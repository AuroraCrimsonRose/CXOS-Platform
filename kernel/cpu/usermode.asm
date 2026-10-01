; /kernel/cpu/usermode.asm
; Aurora Tejeda / CATX Systems LLC
; Ring 3 entry + syscall stub + return-to-kernel path.

bits 32

global enter_usermode
global syscall_stub
global return_to_kernel
extern syscall_dispatch     ; C: int syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2)

; ---------------------------------------------------------------------------
; int enter_usermode(uint32_t entry_eip, uint32_t user_esp, uint32_t *save_slot)
;   save_slot points at a 2-word per-process area: [0]=saved kernel esp,
;   [1]=saved kernel eflags. Using per-process storage (instead of a global)
;   lets multiple ring-3 processes be in flight at once (preemption during
;   ring 3): each has its own return state.
;   Returns ONLY when user code invokes SYS_EXIT (via return_to_kernel).
; ---------------------------------------------------------------------------
enter_usermode:
    push ebp
    push ebx
    push esi
    push edi                 ; save callee-saved regs (cdecl)

    mov edi, [esp + 28]      ; save_slot (4 saved*4 + ret(4) + arg1(4) + arg2(4) = 28)
    pushfd                   ; save the kernel's EFLAGS (esp. the IF state)
    pop eax
    mov [edi + 4], eax       ; save_slot[1] = eflags
    mov [edi], esp           ; save_slot[0] = kernel esp

    mov eax, [esp + 20]      ; entry_eip
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

; return_to_kernel(int retval, uint32_t *save_slot): restore the per-process
; kernel stack + callee regs from save_slot, and return from enter_usermode.
return_to_kernel:
    mov eax, [esp + 4]       ; retval (kept in eax through to the ret)
    mov edi, [esp + 8]       ; save_slot
    mov ecx, [edi + 4]       ; ecx = saved kernel eflags
    mov esp, [edi]           ; restore kernel esp = save_slot[0]
    mov bx, 0x10             ; kernel data
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
    pop edi
    pop esi
    pop ebx
    pop ebp
    push ecx                 ; restore the kernel's original EFLAGS
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
; (per-process ring-3 return state now lives in the process struct, passed to
;  enter_usermode/return_to_kernel as save_slot - no globals needed)

; ---------------------------------------------------------------------------
; ---------------------------------------------------------------------------
; user_blob - a tiny ring-3 demo routine, copied into the user code page by the
; kernel. Position-independent: it uses only `int 0x80` and reads its one
; argument off the user stack (no relative calls/jumps that would break when
; relocated). The kernel seeds the message pointer at the top of the user stack
; before entry, so [esp] holds it on the first instruction.
;
;   SYS_WRITE(ebx = msg)   then   SYS_EXIT(0)
; ---------------------------------------------------------------------------
section .text
global user_blob_start
global user_blob_end
user_blob_start:
    mov ebx, [esp]      ; arg1: msg pointer (seeded by kernel at top of user stack)
    xor ecx, ecx        ; arg2: length 0 = bounded NUL-scan
    mov eax, 0x30       ; SYS_CONSOLE_WRITE
    int 0x80
    mov eax, 0          ; SYS_EXIT
    xor ebx, ebx
    int 0x80
.hang:
    jmp .hang           ; never reached (SYS_EXIT does not return to user)
user_blob_end:

; ---------------------------------------------------------------------------
; user_blob_busy - a longer-running ring-3 routine: it writes its message, then
; spins in a long busy loop (staying in ring 3 the whole time so the timer can
; PREEMPT it mid-execution), writes again, and exits. Used to demonstrate
; preemption during ring 3 (multiple of these interleave under the timer).
; The message pointer is seeded at [esp] by the kernel.
; ---------------------------------------------------------------------------
global user_blob_busy_start
global user_blob_busy_end
user_blob_busy_start:
    mov esi, [esp]      ; save msg pointer in esi (callee-ish; we control it)
    mov edi, 3          ; repeat count
.loop:
    mov ebx, esi        ; arg1: msg
    xor ecx, ecx        ; arg2: NUL-scan
    mov eax, 0x30       ; SYS_CONSOLE_WRITE
    int 0x80
    ; busy work in RING 3 (preemptible): a big spin
    mov ecx, 0x02000000
.spin:
    dec ecx
    jnz .spin
    dec edi
    jnz .loop
    mov eax, 0          ; SYS_EXIT
    xor ebx, ebx
    int 0x80
.bhang:
    jmp .bhang
user_blob_busy_end: