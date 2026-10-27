; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
; /kernel/arch/x86_64/usermode.asm
; Aurora Tejeda / CATX Systems
;
; arch/usermode.h on x86-64: entering ring 3, the syscall gate, leaving ring 3
; for good, and the fault-recoverable user copy (cpu/usermode.h). Then the
; ring-3 routines the arch self-tests run (ktest_arch.c).
;
; The calling convention, until step 6 fixes it in the ABI: the number in
; rax, the arguments in rdi, rsi, rdx, r10 and r8, whole (a u64 is one
; register here, step 6a2d), the result in rax. syscall itself takes rcx (the return RIP) and
; r11 (RFLAGS); every other register comes back as ring 3 left it.
;
; Uniprocessor: the syscall entry parks the user's RSP in one global for two
; instructions. With SMP it moves to per-CPU data reached through swapgs.

bits 64
default rel

%define USER_DATA_SEL  0x1B           ; gdt.h
%define USER_CODE_SEL  0x23
%define KERNEL_DATA    0x10
%define TSS64_RSP0_OFFSET 4

%define SYS_EXIT          0x00        ; cxk_abi.h
%define SYS_YIELD         0x01
%define SYS_GETPID        0x02
%define SYS_CONSOLE_WRITE 0x30

section .text

global enter_usermode
global return_to_kernel
global syscall64_entry
global syscall_trapped
extern syscall_dispatch     ; int syscall_dispatch(uint32_t num, uintptr_t a1, ..., uintptr_t a5)
extern gdt64_tss            ; gdt.c

; ---------------------------------------------------------------------------
; int enter_usermode(uintptr_t entry, uintptr_t user_sp, uintptr_t *save_slot)
;   save_slot[0] = the kernel RSP to come back to, [1] = the kernel's RFLAGS,
;   per thread (thread_current_usave), as on x86-32. Returns only through
;   return_to_kernel: SYS_EXIT, or a fault in ring 3.
; ---------------------------------------------------------------------------
enter_usermode:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15                 ; the SysV callee-saved registers
    pushfq
    pop rax
    mov [rdx + 8], rax       ; save_slot[1] = rflags (the IF state above all)
    mov [rdx], rsp           ; save_slot[0] = kernel rsp

    cli
    push USER_DATA_SEL       ; SS
    push rsi                 ; RSP
    push 0x202               ; RFLAGS: IF, and the reserved bit 1
    push USER_CODE_SEL       ; CS
    push rdi                 ; RIP

    ; Nothing of the kernel's goes up: ring 3 starts with every register zero.
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq                    ; -> ring 3

; return_to_kernel(int retval, uintptr_t *save_slot): back onto the stack
; enter_usermode saved, and return retval from it. Whatever the syscall or the
; fault left above that point on this stack is abandoned.
return_to_kernel:
    mov eax, edi             ; retval
    mov rcx, [rsi + 8]       ; the kernel's RFLAGS
    mov rsp, [rsi]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    push rcx
    popfq
    ret

; ---------------------------------------------------------------------------
; syscall64_entry - LSTAR. The CPU has put the return RIP in rcx and RFLAGS in
; r11, loaded the kernel's CS and SS from STAR, and cleared what FMASK names
; (IF, TF, DF, AC, NT, IOPL; cpu.c). RSP is still ring 3's, so the first two
; instructions move to RSP0 before anything is pushed. An NMI, #DB or #MC
; between them runs on its own IST stack (gdt.h); nothing else can arrive.
;
; Like x86-32's int 0x80 gate, the call runs with interrupts off, and a call
; that blocks switches away from here and comes back the same way.
; ---------------------------------------------------------------------------
syscall64_entry:
    mov [syscall64_user_rsp], rsp
    mov rsp, [gdt64_tss + TSS64_RSP0_OFFSET]
    push qword [syscall64_user_rsp]
    push r11                 ; user RFLAGS
    push rcx                 ; user RIP

    lock inc dword [syscall_trapped]   ; a witness for the recording point

    ; The registers the C call may change, given back as they were. rbx, rbp
    ; and r12-r15 the call keeps itself.
    push rdi
    push rsi
    push rdx
    push r8
    push r9
    push r10
    sub rsp, 8               ; 9 pushes from a 16-byte RSP0: align the call

    ; Ring 3's rdi, rsi, rdx, r10, r8 (CX_SYSCALLS_IPC.md §4) into the C
    ; call's rsi, rdx, rcx, r8, r9, all 64 bits of each (syscall_dispatch
    ; takes uintptr_t), each moved before its source is reused.
    mov r9, r8               ; a5
    mov r8, r10              ; a4
    mov rcx, rdx             ; a3
    mov rdx, rsi             ; a2
    mov rsi, rdi             ; a1
    mov edi, eax             ; num
    call syscall_dispatch
    movsxd rax, eax          ; an error is negative in all 64 bits

    cli                      ; a call that blocked can come back with IF set
    add rsp, 8
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rsi
    pop rdi

    ; SS as syscall left it. A switch made inside an interrupt from ring 3 runs
    ; on with the null SS that interrupt loaded, and AMD's sysret then gives
    ; ring 3 a selector with unusable attributes, so its first push faults.
    ; QEMU does not model it; the rig is an AMD. (Linux reloads SS on switch
    ; for X86_BUG_SYSRET_SS_ATTRS.)
    mov ecx, KERNEL_DATA
    mov ss, ecx

    pop rcx                  ; user RIP: canonical, since paging_map_user
                             ; never maps the user half's last page
    pop r11                  ; user RFLAGS
    pop rsp                  ; user RSP
    o64 sysret

; ---------------------------------------------------------------------------
; int user_copy_bytes(void *dst, const void *src, uint32_t len)
;   0 = copied, 1 = a page fault was taken and recovered from.
;
; x86-32's usermode.asm explains the design. A fault anywhere between
; usercopy_start and usercopy_end on a user address returns through
; usercopy_trampoline instead (idt.c). Nothing is pushed here, so the landing
; pad has nothing to undo: a same-ring fault resumes on the stack it left.
; On a recovered fault the destination is partly written.
; ---------------------------------------------------------------------------
global user_copy_bytes
global usercopy_start
global usercopy_end
global usercopy_trampoline

usercopy_start:
user_copy_bytes:
    mov ecx, edx             ; len
    cld
    rep movsb                ; the only instruction here that can fault
    xor eax, eax
    ret
usercopy_end:

usercopy_trampoline:
    mov eax, 1
    ret

section .bss
alignb 8
syscall64_user_rsp: resq 1
; Traps taken through the syscall entry (syscall_trapped_count), counted
; before C runs, so a test can tell a call that reached the recording point
; from one that only reached the gate.
syscall_trapped:    resd 1

; ---------------------------------------------------------------------------
; The ring-3 routines of the arch self-tests (ktest_arch.c). Each is copied to
; the start of a user code page, so each is position-independent: RIP-relative
; addresses inside itself only. The test's one argument, if any, is at [rsp].
; ---------------------------------------------------------------------------
section .text

; Exits with CS | SS << 8 | IF << 16 | (a register not zero on entry) << 17:
; 0x11B23 when ring 3 runs at CPL 3 on the user segments with interrupts on,
; and enter_usermode handed up nothing of the kernel's.
global user64_cpl_start, user64_cpl_end
user64_cpl_start:
    or rax, rbx
    or rax, rcx
    or rax, rdx
    or rax, rsi
    or rax, rdi
    or rax, rbp
    or rax, r8
    or rax, r9
    or rax, r10
    or rax, r11
    or rax, r12
    or rax, r13
    or rax, r14
    or rax, r15
    neg rax
    sbb r8d, r8d             ; all ones if any register was not zero
    and r8d, 1 << 17
    mov eax, cs
    mov edi, eax
    mov eax, ss
    shl eax, 8
    or edi, eax
    pushfq
    pop rax
    shr eax, 9
    and eax, 1
    shl eax, 16
    or edi, eax
    or edi, r8d
    mov eax, SYS_EXIT
    syscall
.hang:
    jmp .hang
user64_cpl_end:

; Fills every register syscall does not take with a pattern, calls SYS_YIELD
; and SYS_GETPID, and exits with mask << 16 | pid: mask bit n set for each
; pattern lost (0-11), for an r11 without IF (12), and for an rcx that is not
; the return address (13).
%macro KEEP 3                ; register, pattern, bit
    mov rcx, %2
    cmp %1, rcx
    je %%ok
    or eax, 1 << %3
%%ok:
%endmacro

global user64_regs_start, user64_regs_end
user64_regs_start:
    mov rbx, 0x1111111111111111
    mov rbp, 0x2222222222222222
    mov rdx, 0x3333333333333333
    mov rsi, 0x4444444444444444
    mov rdi, 0x5555555555555555
    mov r8,  0x6666666666666666
    mov r9,  0x7777777777777777
    mov r10, 0x8888888888888888
    mov r12, 0x9999999999999999
    mov r13, 0xAAAAAAAAAAAAAAAA
    mov r14, 0xBBBBBBBBBBBBBBBB
    mov r15, 0xCCCCCCCCCCCCCCCC
    mov eax, SYS_YIELD
    syscall
    mov eax, SYS_GETPID
    syscall
.after:
    push rax                 ; the pid
    push rcx
    push r11
    xor eax, eax
    KEEP rbx, 0x1111111111111111, 0
    KEEP rbp, 0x2222222222222222, 1
    KEEP rdx, 0x3333333333333333, 2
    KEEP rsi, 0x4444444444444444, 3
    KEEP rdi, 0x5555555555555555, 4
    KEEP r8,  0x6666666666666666, 5
    KEEP r9,  0x7777777777777777, 6
    KEEP r10, 0x8888888888888888, 7
    KEEP r12, 0x9999999999999999, 8
    KEEP r13, 0xAAAAAAAAAAAAAAAA, 9
    KEEP r14, 0xBBBBBBBBBBBBBBBB, 10
    KEEP r15, 0xCCCCCCCCCCCCCCCC, 11
    pop r11
    test r11d, 0x200
    jnz .if_ok
    or eax, 1 << 12
.if_ok:
    pop rcx
    lea rdx, [.after]
    cmp rcx, rdx
    je .rcx_ok
    or eax, 1 << 13
.rcx_ok:
    pop rdx                  ; the pid
    shl eax, 16
    movzx edi, dx
    or edi, eax
    mov eax, SYS_EXIT
    syscall
.hang:
    jmp .hang
user64_regs_end:

; Writes its message, then tries 4 bytes at 0x80000000, which nothing maps,
; and exits with first result & 0xFFFF | (second & 0xFF) << 16.
global user64_write_start, user64_write_end, user64_write_msg, user64_write_msg_end
user64_write_start:
    lea rdi, [user64_write_msg]
    mov esi, user64_write_msg_end - user64_write_msg
    mov eax, SYS_CONSOLE_WRITE
    syscall
    mov r12d, eax
    mov edi, 0x80000000
    mov esi, 4
    mov eax, SYS_CONSOLE_WRITE
    syscall
    and eax, 0xFF
    shl eax, 16
    movzx edi, r12w
    or edi, eax
    mov rsi, 0xA2000000C0DE0002   ; a2..a5: unused by SYS_EXIT, and read back
    mov rdx, 0xA3000000C0DE0003   ; from the recording point by t_ring3_write,
    mov r10, 0xA4000000C0DE0004   ; so every register the gate is meant to carry
    mov r8, 0xA5000000C0DE0005    ; is seen to arrive, its high half included
    mov eax, SYS_EXIT
    syscall
.hang:
    jmp .hang
user64_write_msg:
    db "                      -> ring 3 SYS_CONSOLE_WRITE ok", 10
user64_write_msg_end:
user64_write_end:

; An invalid opcode at once.
global user64_ud2_start, user64_ud2_end
user64_ud2_start:
    ud2
user64_ud2_end:

; Reads a byte at the address in [rsp]: a kernel address, so a page fault.
global user64_read_start, user64_read_end
user64_read_start:
    mov rax, [rsp]
    mov al, [rax]
    mov edi, 1
    mov eax, SYS_EXIT
    syscall
user64_read_end:

; Jumps to its own stack, which is no-execute.
global user64_exec_stack_start, user64_exec_stack_end
user64_exec_stack_start:
    jmp rsp
user64_exec_stack_end:

; Writes to its own code, which is not writable.
global user64_write_code_start, user64_write_code_end
user64_write_code_start:
    lea rax, [user64_write_code_start]
    mov byte [rax], 0xCC
    mov edi, 1
    mov eax, SYS_EXIT
    syscall
user64_write_code_end:

; Pushes until the stack runs into the unmapped page below it.
global user64_overflow_start, user64_overflow_end
user64_overflow_start:
    push rax
    jmp user64_overflow_start
user64_overflow_end:

; Counts itself in at the shared page whose address is in [rsp], spins with no
; syscall until the kernel writes a nonzero word there, and exits with that
; word. It can only finish if the tick preempts it in ring 3, since the
; kernel thread that writes the word runs only when this one is switched out.
global user64_spin_start, user64_spin_end
user64_spin_start:
    mov rbx, [rsp]
    lock inc dword [rbx + 4]
.spin:
    pause
    mov edi, [rbx]
    test edi, edi
    jz .spin
    mov eax, SYS_EXIT
    syscall
user64_spin_end:

; Packs ds, es, fs and gs one per byte, makes a SYS_YIELD (which switches
; away and back), packs them again, and exits with CS | SS << 8 if the two
; agree and 0 if not: 0x1B23 when the call gave ring 3 back everything it
; had (ktest_arch_user_segments_kept).
%macro PACK_SEGS 2            ; the register, and its low byte
    mov eax, gs
    movzx %1, al
    shl %1, 8
    mov eax, fs
    mov %2, al
    shl %1, 8
    mov eax, es
    mov %2, al
    shl %1, 8
    mov eax, ds
    mov %2, al
%endmacro
global user64_segs_start, user64_segs_end
user64_segs_start:
    PACK_SEGS r12d, r12b
    mov eax, SYS_YIELD
    syscall
    PACK_SEGS r13d, r13b
    mov eax, cs
    mov edi, eax
    mov eax, ss
    shl eax, 8
    or edi, eax
    cmp r12d, r13d
    je .same
    xor edi, edi
.same:
    mov eax, SYS_EXIT
    syscall
.hang:
    jmp .hang
user64_segs_end:

; ---------------------------------------------------------------------------
; usermode.c's ring-3 routines, as x86-32's (arch/x86_32/usermode.asm) in
; this convention. The kernel seeds a 32-bit message pointer at [rsp].
;
; user_blob: SYS_CONSOLE_WRITE(msg, 0 = bounded NUL-scan), then SYS_EXIT(0).
; ---------------------------------------------------------------------------
global user_blob_start, user_blob_end
user_blob_start:
    mov edi, [rsp]
    xor esi, esi
    mov eax, SYS_CONSOLE_WRITE
    syscall
    xor edi, edi
    mov esi, 0xC0DE0002      ; a2..a5 for test_sysrec, as in x86-32's user_blob
    mov edx, 0xC0DE0003
    mov r10d, 0xC0DE0004
    mov r8d, 0xC0DE0005
    mov eax, SYS_EXIT
    syscall
.hang:
    jmp .hang
user_blob_end:

; user_blob_busy: writes its message, spins in ring 3 long enough for the tick
; to preempt it, three times over, then exits.
global user_blob_busy_start, user_blob_busy_end
user_blob_busy_start:
    mov r12d, [rsp]          ; the message, kept across the calls
    mov r13d, 3
.loop:
    mov edi, r12d
    xor esi, esi
    mov eax, SYS_CONSOLE_WRITE
    syscall
    mov ecx, 0x02000000
.spin:
    dec ecx
    jnz .spin
    dec r13d
    jnz .loop
    xor edi, edi
    mov eax, SYS_EXIT
    syscall
.hang:
    jmp .hang
user_blob_busy_end:
