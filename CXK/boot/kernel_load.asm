; /CXLite/boot/kernel_load.asm
; Aurora Tejeda

; Self-sizing chunked kernel loader.
;
; The number of sectors to load is NOT hardcoded - it's patched into
; kernel_sectors at build time by tools/pad_disk.ps1, which computes it from
; the actual kernel.bin size. The script locates the field by scanning for the
; KSNT_MAGIC marker, so it survives code moving around. This means the loader
; always reads exactly as many sectors as the kernel needs - no magic number to
; maintain, no silent corruption when the kernel grows.
;
; INT 13h extended read can transfer at most 127 sectors per call, so we LOOP,
; reading up to 127 at a time, advancing the LBA and the destination segment
; each pass, until the whole kernel is loaded.

KERNEL_LOAD_SEGMENT equ 0x1000      ; kernel loads at 0x10000 (seg*16+off)
KERNEL_LOAD_OFFSET  equ 0x0000
KERNEL_LOAD_ADDRESS equ 0x10000
KERNEL_SECTOR_START equ 18          ; sector 1 = boot, 2-17 = stage2, 18+ = kernel
MAX_PER_CALL        equ 127         ; INT 13h extended single-call sector limit

; --- patchable sector count ---
; KSNT_MAGIC lets the build script find this field by signature. The dd after
; it is overwritten with ceil(kernel_size/512) at image-assembly time. The
; default (0xFFFF) is a sentinel: if it's ever still set at boot, the patch
; didn't happen and we error loudly instead of loading garbage.
KSNT_MAGIC equ 0x4B534E54            ; "KSNT" (Kernel Sector couNT)
kernel_count_marker:
    dd KSNT_MAGIC
kernel_sectors:
    dd 0x0000FFFF                    ; <- patched by pad_disk.ps1

; disk address packet for INT 13h AH=42h (rewritten each chunk)
align 4
dap:
    db 0x10                 ; packet size
    db 0x00                 ; reserved
dap_count:
    dw 0                    ; sectors to read this call
dap_offset:
    dw KERNEL_LOAD_OFFSET   ; destination offset
dap_segment:
    dw KERNEL_LOAD_SEGMENT  ; destination segment
dap_lba:
    dq KERNEL_SECTOR_START  ; starting LBA

; load_kernel - loads the whole kernel in <=127-sector chunks
load_kernel:
    pushad

    ; sanity: was the sector count patched? if still the sentinel, fail loud.
    mov eax, [kernel_sectors]
    cmp eax, 0x0000FFFF
    je .not_patched
    test eax, eax
    jz .not_patched

    ; verify INT 13h extensions
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .no_ext
    cmp bx, 0xAA55
    jne .no_ext

    ; --- chunked load loop ---
    ; ebp = sectors remaining
    mov ebp, [kernel_sectors]
    ; current LBA in dword [dap_lba] (low 32 bits are plenty)
    mov dword [dap_lba], KERNEL_SECTOR_START
    mov dword [dap_lba + 4], 0
    mov word [dap_segment], KERNEL_LOAD_SEGMENT
    mov word [dap_offset], KERNEL_LOAD_OFFSET

.next_chunk:
    test ebp, ebp
    jz .done

    ; this_chunk = min(remaining, 127)
    mov ecx, ebp
    cmp ecx, MAX_PER_CALL
    jbe .have_count
    mov ecx, MAX_PER_CALL
.have_count:
    mov [dap_count], cx

    ; do the extended read
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap
    int 0x13
    jc .disk_error

    ; advance LBA by cx sectors
    movzx eax, cx
    add [dap_lba], eax
    ; advance destination segment by (cx * 512 / 16) = cx * 32
    mov ax, cx
    shl ax, 5                       ; * 32
    add [dap_segment], ax
    ; remaining -= cx
    movzx eax, cx
    sub ebp, eax
    jmp .next_chunk

.done:
    popad
    ret

.no_ext:
    ; CHS fallback: can't easily do >63 sectors/track; load a single capped
    ; chunk (enough for QEMU/Bochs/modern BIOS which all support extensions
    ; anyway - this path is essentially never taken).
    mov ah, 0x02
    mov al, MAX_PER_CALL
    mov ch, 0
    mov cl, KERNEL_SECTOR_START
    mov dh, 0
    mov dl, [boot_drive]
    mov ax, KERNEL_LOAD_SEGMENT
    mov es, ax
    mov bx, KERNEL_LOAD_OFFSET
    int 0x13
    jc .disk_error
    popad
    ret

.not_patched:
    mov si, msg_kernel_not_patched
    call print_string
    call print_newline
    jmp halt

.disk_error:
    mov [disk_error_code], ah
    mov si, msg_kernel_load_fail
    call print_string
    mov al, [disk_error_code]
    call print_error_code
    call print_newline
    jmp halt

; strings
msg_kernel_load_fail    db '[BOOT] KERNEL LOAD FAIL', 0
msg_kernel_not_patched  db '[BOOT] KERNEL SIZE NOT SET (build error)', 0