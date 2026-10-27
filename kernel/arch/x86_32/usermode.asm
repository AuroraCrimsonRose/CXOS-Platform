; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /kernel/arch/x86_32/usermode.asm
; Aurora Tejeda / CATX Systems
; Ring 3 entry + syscall stub + return-to-kernel path.

bits 32

global enter_usermode
global syscall_stub
global return_to_kernel
global syscall_trapped
extern syscall_dispatch     ; C: int syscall_dispatch(uint32_t num, uintptr_t a1, ..., uintptr_t a5); 32 bits each here

; ---------------------------------------------------------------------------
; int enter_usermode(uint32_t entry_eip, uint32_t user_esp, uint32_t *save_slot,
;                    uint32_t sysbase)
;   save_slot points at a 2-word per-process area: [0]=saved kernel esp,
;   [1]=saved kernel eflags. Using per-process storage (instead of a global)
;   lets multiple ring-3 processes be in flight at once (preemption during
;   ring 3): each has its own return state.
;   Ring 3 starts with every register zero but esi, which holds sysbase, the
;   system page's base (cxk_abi.h). Nothing of the kernel's goes up.
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
    mov esi, [esp + 32]      ; sysbase, carried up to ring 3

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
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor edi, edi
    xor ebp, ebp
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
    ; The caller's segment registers, given back on the way out. Left as the
    ; kernel's, the iretd to ring 3 loads each with NULL (their DPL 0 is below
    ; CPL 3), and the program's next DS access takes #GP(0) - which is what
    ; pids 2 and 3 did on the FX, the first DS access after a syscall. QEMU
    ; nulls the selector but keeps the old base and does not check it, so it
    ; never showed there; ktest_arch_user_segments_kept reads the selectors.
    push ds
    push es
    push fs
    push gs
    mov bp, 0x10             ; kernel data segments
    mov ds, bp
    mov es, bp
    mov fs, bp
    mov gs, bp

    lock inc dword [syscall_trapped]   ; a witness for the recording point

    push edi                 ; a5
    push esi                 ; a4
    push edx                 ; a3
    push ecx                 ; a2
    push ebx                 ; a1
    push eax                 ; syscall number
    call syscall_dispatch
    add esp, 24

    ; store return value into saved eax (pushad: eax at [esp+28], above the
    ; four segment registers)
    mov [esp + 16 + 28], eax

    ; Interrupts off before the selectors go back: a syscall that blocked can
    ; return with IF set, and an IRQ landing mid-pop would restore ES/FS/GS
    ; from the still-kernel DS - nulled by the iretd, #GP on the next movs.
    cli
    pop gs
    pop fs
    pop es
    pop ds
    popad
    sti
    iretd

; ---------------------------------------------------------------------------
; int user_copy_bytes(void *dst, const void *src, uint32_t len)
;   0 = copied, 1 = a page fault was taken and recovered from.
;
; The fault-recoverable copy primitive behind user_copy_out / user_copy_in
; (security review §6). Both pointers are range-checked by the caller; this
; routine exists for the case the range check cannot cover - the mapping
; changing, or never having been what the page tables said - where the copy
; itself must fail rather than take an unrecoverable ring-0 fault.
;
; Everything between usercopy_start and usercopy_end is the recoverable
; region. The page-fault handler compares the faulting EIP against it and, on
; a match, redirects the return to usercopy_trampoline below (idt.c). There is
; no armed flag anywhere: recovery is a property of *where the fault happened*,
; so it cannot be left switched on and swallow an unrelated kernel fault.
; Redox does it this way (__usercopy_start/__usercopy_end, src/memory/mod.rs)
; for that reason; Haiku and Mach instead keep a per-thread fault-handler slot
; that each copy arms and restores, which works but has a failure mode this
; does not.
;
; On a recovered fault the destination is PARTIALLY WRITTEN - `rep movsb`
; stops where it faulted. Callers must treat it as undefined, which every
; caller here does by returning E_BAD_ADDRESS and copying nothing further.
; ---------------------------------------------------------------------------
global user_copy_bytes
global usercopy_start
global usercopy_end
global usercopy_trampoline

usercopy_start:
user_copy_bytes:
    push edi
    push esi                 ; see usercopy_trampoline: it undoes exactly these
    mov edi, [esp + 12]      ; dst   (2 pushes = 8, + ret = 12)
    mov esi, [esp + 16]      ; src
    mov ecx, [esp + 20]      ; len
    cld                      ; forward; never assume DF
    rep movsb                ; the only instruction here that can fault
    pop esi
    pop edi
    xor eax, eax             ; 0 = copied
    ret
usercopy_end:

; The landing pad the page-fault handler returns to instead of the faulting
; instruction. A same-ring fault does not save or restore ESP, so ESP is still
; exactly what it was mid-copy and the prologue's two pushes are still on the
; stack: undo them and return the fault indicator. Keep in lockstep with the
; prologue above - this is a hand-maintained pairing, and nothing but a test
; will notice if it drifts.
usercopy_trampoline:
    pop esi
    pop edi
    mov eax, 1               ; 1 = faulted, recovered
    ret

section .bss
; Traps taken through int 0x80 (C: syscall_trapped_count, usermode.h). Counted
; here, before C runs, so the self-test can tell a call that reached the
; recording point from one that only reached the gate.
syscall_trapped: resd 1
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
    mov ecx, 0xC0DE0002 ; a2..a5: unused by SYS_EXIT, and read back from the
    mov edx, 0xC0DE0003 ; recording point by test_sysrec, so every register
    mov esi, 0xC0DE0004 ; the gate is meant to carry is seen to arrive
    mov edi, 0xC0DE0005
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

; ---------------------------------------------------------------------------
; user_blob_segs - makes one syscall, then exits with its four data segment
; selectors packed one per byte (gs fs es ds, high to low). After a syscall
; that hands them back, that is 0x23232323; a syscall that leaves the kernel's
; in place gets them nulled by the iretd, and reads 0.
; ---------------------------------------------------------------------------
global user_blob_segs_start
global user_blob_segs_end
user_blob_segs_start:
    mov eax, 0x01       ; SYS_YIELD
    int 0x80
    xor ebx, ebx
    mov ax, gs
    mov bl, al
    shl ebx, 8
    mov ax, fs
    mov bl, al
    shl ebx, 8
    mov ax, es
    mov bl, al
    shl ebx, 8
    mov ax, ds
    mov bl, al
    xor eax, eax        ; SYS_EXIT(ebx)
    int 0x80
.shang:
    jmp .shang
user_blob_segs_end:

; ---------------------------------------------------------------------------
; user_sysp_call / user_sysp_write - through the system page, whose base
; enter_usermode leaves in esi. user_sysp_call calls the SYS_GETPID stub and
; exits, through the SYS_EXIT stub, with the pid + 0x100. user_sysp_write
; stores to the page, which must fault: it is never writable from ring 3.
; Stub n is at esi + n * 16 (SYSPAGE_STUB_SIZE).
; ---------------------------------------------------------------------------
global user_sysp_call_start, user_sysp_call_end
user_sysp_call_start:
    lea eax, [esi + 0x02 * 16]   ; SYS_GETPID
    call eax
    lea ebx, [eax + 0x100]
    lea eax, [esi + 0x00 * 16]   ; SYS_EXIT(ebx)
    call eax
    ud2                          ; a stub that returned ends it, as a fault
user_sysp_call_end:

global user_sysp_write_start, user_sysp_write_end
user_sysp_write_start:
    mov byte [esi + 0xFFF], 0xC3 ; an int3 slot: the exit stub stays whole
    xor ebx, ebx
    lea eax, [esi + 0x00 * 16]   ; SYS_EXIT(0): only if the store went through
    call eax
    ud2
user_sysp_write_end:
