; /CXLite/boot/vbe.asm
; Aurora Tejeda
;
; VBE (VESA BIOS Extensions) graphics mode setup. MUST run in real mode
; (uses int 0x10), so it's called from stage2 BEFORE entering protected mode.
;
; Strategy: query the VBE mode list, walk it looking for a mode that matches
; one of our preferred (width,height,bpp) combos with a LINEAR framebuffer.
; If found, set it and stash the framebuffer info for the kernel. If nothing
; matches (or VBE is absent), leave VBE_VALID = 0 and the kernel stays in VGA
; text mode (safe fallback - we never end up at an undebuggable black screen).
;
; The kernel reads these fixed low-memory addresses:
;   VBE_VALID   (byte)  1 = framebuffer set, 0 = stay in text mode
;   VBE_WIDTH   (word)  pixels across
;   VBE_HEIGHT  (word)  pixels down
;   VBE_BPP     (byte)  bits per pixel (8, 16, or 32)
;   VBE_PITCH   (word)  bytes per scanline
;   VBE_FB      (dword) physical framebuffer address

; ---- where we stash results for the kernel (just past the other boot vars) ----
VBE_VALID   equ 0x1C40
VBE_WIDTH   equ 0x1C42
VBE_HEIGHT  equ 0x1C44
VBE_BPP     equ 0x1C46
VBE_PITCH   equ 0x1C48
VBE_FB      equ 0x1C4A

; ---- scratch buffers (real-mode addressable) ----
VBE_INFO_BUF  equ 0x2000      ; 512-byte VBE controller info block
VBE_MODE_BUF  equ 0x2200      ; 256-byte VBE mode info block

; preferred modes table: width, height, bpp (try in order; first match wins).
; we try 32bpp first (easiest to draw), then 16, then 8, at a few resolutions.
vbe_pref_table:
    dw 1024, 768, 32
    dw 800,  600, 32
    dw 1024, 768, 16
    dw 800,  600, 16
    dw 1024, 768, 8
    dw 800,  600, 8
    dw 0, 0, 0           ; terminator

; set_video_mode: main entry. tries to set a preferred VBE mode.
set_video_mode:
    pusha
    push es
    push fs
    push ds

    ; ensure ds=0 so all our fixed-address and local accesses resolve correctly
    xor ax, ax
    mov ds, ax

    ; default: not valid (text-mode fallback)
    mov byte [VBE_VALID], 0

    ; --- get VBE controller info (function 0x4F00) ---
    mov ax, 0
    mov es, ax
    mov di, VBE_INFO_BUF
    ; signature "VBE2" requests VBE 2.0+ info (gets us the mode list + LFB)
    mov dword [es:di], 'VBE2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F          ; AL=4F (supported), AH=00 (success)
    jne .done               ; no VBE -> stay text mode

    ; the controller info block holds a far pointer (off:seg) at offset 0x0E
    ; to the list of supported mode numbers (terminated by 0xFFFF).
    ; we walk that list, and for each mode query its info and compare.

.try_each_pref:
    ; outer loop over our preference table
    mov si, vbe_pref_table
.pref_loop:
    mov ax, [si]            ; desired width
    test ax, ax
    jz .done                ; table terminator -> no match found, fallback
    mov [.want_w], ax
    mov ax, [si+2]
    mov [.want_h], ax
    mov ax, [si+4]
    mov [.want_bpp], ax

    ; walk the mode list for this preference
    ; reload mode-list far pointer each time
    mov ax, [VBE_INFO_BUF + 0x10]   ; segment of mode list
    mov fs, ax
    mov bx, [VBE_INFO_BUF + 0x0E]   ; offset of mode list

.mode_loop:
    mov dx, [fs:bx]         ; next mode number
    cmp dx, 0xFFFF
    je .next_pref           ; end of list, try next preference
    add bx, 2
    mov [.cur_mode], dx
    mov [.saved_bx], bx
    mov [.saved_fs], fs

    ; --- get this mode's info (function 0x4F01) ---
    mov ax, 0
    mov es, ax
    mov di, VBE_MODE_BUF
    mov cx, dx              ; mode number
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .restore_continue

    ; mode info block fields we care about:
    ;   0x00 word  mode attributes (bit 7 = linear framebuffer available)
    ;   0x10 word  bytes per scanline (pitch)
    ;   0x12 word  width  (X resolution)
    ;   0x14 word  height (Y resolution)
    ;   0x19 byte  bits per pixel
    ;   0x28 dword physical framebuffer address (linear)
    mov ax, [VBE_MODE_BUF + 0x00]
    test ax, 0x80           ; linear framebuffer supported?
    jz .restore_continue

    mov ax, [VBE_MODE_BUF + 0x12]   ; width
    cmp ax, [.want_w]
    jne .restore_continue
    mov ax, [VBE_MODE_BUF + 0x14]   ; height
    cmp ax, [.want_h]
    jne .restore_continue
    xor ax, ax
    mov al, [VBE_MODE_BUF + 0x19]   ; bpp
    cmp ax, [.want_bpp]
    jne .restore_continue

    ; --- match! set this mode (function 0x4F02), with LFB bit (0x4000) ---
    mov bx, [.cur_mode]
    or bx, 0x4000           ; request linear framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .restore_continue   ; set failed, keep looking

    ; --- success: stash framebuffer info for the kernel ---
    mov ax, [VBE_MODE_BUF + 0x12]
    mov [VBE_WIDTH], ax
    mov ax, [VBE_MODE_BUF + 0x14]
    mov [VBE_HEIGHT], ax
    xor ax, ax
    mov al, [VBE_MODE_BUF + 0x19]
    mov [VBE_BPP], ax
    mov ax, [VBE_MODE_BUF + 0x10]
    mov [VBE_PITCH], ax
    mov eax, [VBE_MODE_BUF + 0x28]
    mov [VBE_FB], eax
    mov byte [VBE_VALID], 1
    jmp .done

.restore_continue:
    ; restore mode-list walk pointer and continue
    mov fs, [.saved_fs]
    mov bx, [.saved_bx]
    jmp .mode_loop

.next_pref:
    add si, 6               ; next preference entry (3 words)
    jmp .pref_loop

.done:
    pop ds
    pop fs
    pop es
    popa
    ret

; locals (real-mode scratch)
.want_w     dw 0
.want_h     dw 0
.want_bpp   dw 0
.cur_mode   dw 0
.saved_bx   dw 0
.saved_fs   dw 0