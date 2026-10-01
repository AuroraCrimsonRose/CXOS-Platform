; /boot/vbe.asm  -  VBE linear-framebuffer mode set (v5 stage 2)
; Aurora Tejeda / CATX SYSTEMS LLC
;
; Sets a VESA BIOS Extensions (VBE 2.0+) linear-framebuffer graphics mode in
; REAL MODE (BIOS int 10h is gone once we enter protected mode), so stage 2
; calls this just before the pmode switch - after all real-mode text logging,
; because once a graphics mode is live the text console at 0xB8000 is no longer
; displayed.
;
; "Not hardcoded": we do NOT assume fixed VBE mode numbers (they vary by BIOS).
; Instead we ENUMERATE the controller's reported mode list and MATCH each mode
; against a preference-ordered table of (width, height) pairs - all at 16bpp
; 5-6-5 direct color with a linear framebuffer. The FIRST table entry the BIOS
; actually offers wins. Adding resolutions = adding rows to vbe_pref_table, not
; code. (Only one mode is chosen at boot; the rest of the table is the fallback
; chain - reorder to change the effective preference.)
;
; On success: stash a compact info struct at VBE_INFO (read by kernel fb.c).
; On any failure: VBE_VALID byte = 0 and we leave the display in text mode, so
; the kernel's text console stays visible (important for debugging early boots).
;
; Gated by CXK_ENABLE_FB: when 0, set_vbe_mode is a no-op that just clears
; VBE_VALID, guaranteeing the bootloader never touches the video hardware and
; the text-mode fallback is real. The flag MUST match the kernel's config.h -
; wire it from CMake as `nasm -DCXK_ENABLE_FB=<n>` AND `gcc -DCXK_ENABLE_FB=<n>`
; from one CMake variable so both sides agree.

; This file is %included into stage2 AFTER cxexload.asm, which leaves the
; assembler in `bits 32`. set_vbe_mode runs in REAL MODE (called before the
; pmode switch), so it MUST be assembled as 16-bit - declare that explicitly
; here rather than relying on include order. (Without this, the 32-bit encoding
; runs in real mode, the int 10h VBE calls fail, and we silently fall back to
; text mode.)
bits 16

%ifndef CXK_ENABLE_FB
%define CXK_ENABLE_FB 1
%endif

; ----------------------------------------------------------------------------
; Fixed low-memory scratch + handoff addresses (real mode, < 1 MB).
; Clear of: stage1 (0x7C00), stage2 (0x7E00+), the E820 buffer (0x0504..0x0B04).
; ----------------------------------------------------------------------------
VBE_CTRL_BUF   equ 0x1000    ; 512-byte VbeInfoBlock      (0x1000..0x1200), transient
VBE_MODE_BUF   equ 0x1200    ; 256-byte ModeInfoBlock     (0x1200..0x1300), transient

; Persistent 14-byte struct the kernel reads (must match fb.c VBE_* defines):
VBE_INFO       equ 0x1C40
VBE_VALID      equ 0x1C40    ; byte  : 1 = framebuffer active, 0 = stay text mode
VBE_WIDTH      equ 0x1C42    ; word  : pixels
VBE_HEIGHT     equ 0x1C44    ; word  : pixels
VBE_BPP        equ 0x1C46    ; byte  : 16
VBE_PITCH      equ 0x1C48    ; word  : bytes per scanline
VBE_FB         equ 0x1C4A    ; dword : linear framebuffer physical base
                             ; (ends 0x1C4E)

; VbeInfoBlock field offsets (we only need the mode-list far pointer)
VIB_VIDEOMODEPTR equ 0x0E    ; dword far ptr (offset:segment) to the mode list

; ModeInfoBlock field offsets (VBE 2.0/3.0)
MIB_ATTRIBUTES   equ 0x00    ; word
MIB_PITCH        equ 0x10    ; word  : BytesPerScanLine (VBE 1.x / banked)
MIB_WIDTH        equ 0x12    ; word  : XResolution
MIB_HEIGHT       equ 0x14    ; word  : YResolution
MIB_BPP          equ 0x19    ; byte  : BitsPerPixel
MIB_MEMMODEL     equ 0x1B    ; byte  : MemoryModel (6 = direct color)
MIB_RED_MASK     equ 0x1F    ; byte  : RedMaskSize
MIB_GREEN_MASK   equ 0x21    ; byte  : GreenMaskSize
MIB_BLUE_MASK    equ 0x23    ; byte  : BlueMaskSize
MIB_PHYSBASE     equ 0x28    ; dword : PhysBasePtr (LFB physical address)
MIB_LIN_PITCH    equ 0x32    ; word  : LinBytesPerScanLine (VBE 3.0, LFB pitch)

; ModeAttributes bits we require
ATTR_SUPPORTED   equ 0x01    ; bit 0 : mode supported in current config
ATTR_GRAPHICS    equ 0x10    ; bit 4 : graphics (not text) mode
ATTR_LFB         equ 0x80    ; bit 7 : linear framebuffer available
ATTR_NEED        equ (ATTR_SUPPORTED | ATTR_GRAPHICS | ATTR_LFB)

; mode-set control bits (in BX for int 10h AX=4F02h)
SET_LFB_BIT      equ 0x4000  ; bit 14 : use linear framebuffer

; ----------------------------------------------------------------------------
; set_vbe_mode  -  entry point called from stage2. Real mode. Always leaves
; VBE_VALID written (0 or 1); preserves caller registers via pusha/push.
; ----------------------------------------------------------------------------
set_vbe_mode:
    pusha
    push es
    push fs

    ; default to "no framebuffer" so every early-out path is text-mode-safe
    xor ax, ax
    mov es, ax
    mov byte [es:VBE_VALID], 0

%if CXK_ENABLE_FB == 0
    ; framebuffer disabled at build time: never touch the video hardware.
    mov si, msg_vbe_off
    call print_string
    call print_newline
    jmp .ret
%else

    ; --- 1. get VbeInfoBlock (request VBE 2.0 info by stamping 'VBE2') --------
    mov di, VBE_CTRL_BUF
    mov byte [es:di + 0], 'V'
    mov byte [es:di + 1], 'B'
    mov byte [es:di + 2], 'E'
    mov byte [es:di + 3], '2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .fail                    ; VBE not present / call failed
    ; signature should now read 'VESA'
    cmp byte [es:VBE_CTRL_BUF + 0], 'V'
    jne .fail
    cmp byte [es:VBE_CTRL_BUF + 1], 'E'
    jne .fail
    cmp byte [es:VBE_CTRL_BUF + 2], 'S'
    jne .fail
    cmp byte [es:VBE_CTRL_BUF + 3], 'A'
    jne .fail

    ; --- 2. outer loop: walk the preference table ----------------------------
    mov si, vbe_pref_table
.pref_next:
    mov ax, [si]                 ; desired width
    mov dx, [si + 2]             ; desired height
    mov [vbe_want_w], ax
    mov [vbe_want_h], dx
    ; (0,0) terminator -> nothing matched
    or  ax, dx
    jz  .none

    ; reset the mode-list cursor to the list start for this rescan. The list
    ; pointer is a far ptr (offset:segment) in the VbeInfoBlock. We KEEP it in
    ; memory and reload fs from there each step, because int 10h (GetModeInfo)
    ; is not guaranteed to preserve fs - relying on a live fs:bx across the BIOS
    ; call would walk garbage on some BIOSes.
    xor ax, ax
    mov es, ax
    mov ax, [es:VBE_CTRL_BUF + VIB_VIDEOMODEPTR + 2]
    mov [vbe_list_seg], ax
    mov ax, [es:VBE_CTRL_BUF + VIB_VIDEOMODEPTR]
    mov [vbe_list_off], ax

.mode_next:
    mov fs, [vbe_list_seg]       ; reload (BIOS may have clobbered fs)
    mov bx, [vbe_list_off]
    mov cx, [fs:bx]              ; next mode number
    cmp cx, 0xFFFF
    je .pref_advance             ; end of list -> try next preferred res
    add word [vbe_list_off], 2   ; advance cursor in memory (clobber-proof)
    mov [vbe_cur_mode], cx

    ; --- 3. GetModeInfo for this mode ---------------------------------------
    xor ax, ax
    mov es, ax
    mov di, VBE_MODE_BUF
    mov cx, [vbe_cur_mode]
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .mode_next               ; couldn't query -> next mode (cursor advanced)
    xor ax, ax
    mov es, ax                   ; re-assert es=0 (BIOS may clobber es on int 10h)

    ; require: supported + graphics + LFB available
    mov ax, [es:VBE_MODE_BUF + MIB_ATTRIBUTES]
    and ax, ATTR_NEED
    cmp ax, ATTR_NEED
    jne .mode_next
    ; require: direct-color (model 6) in a layout the kernel can actually draw.
    ; The kernel (fb.c: fb_rgb / fb_put_pixel / fb_fill_rect) handles exactly
    ; two: 16bpp 5-6-5 and 32bpp 8-8-8. Accepting only 16bpp was needlessly
    ; narrow - plenty of firmware, especially UEFI CSM, offers no 16bpp LFB mode
    ; at all, and there the search found nothing and fell back to text mode, so
    ; the machine simply had no framebuffer and no GUI.
    cmp byte [es:VBE_MODE_BUF + MIB_MEMMODEL], 6
    jne .mode_next
    mov al, [es:VBE_MODE_BUF + MIB_BPP]
    cmp al, 16
    je .bpp16
    cmp al, 32
    je .bpp32
    jmp .mode_next               ; 8/15/24bpp: kernel cannot draw it
.bpp16:
    cmp byte [es:VBE_MODE_BUF + MIB_RED_MASK], 5
    jne .mode_next               ; reject 5-5-5 (15bpp masquerading as 16)
    cmp byte [es:VBE_MODE_BUF + MIB_GREEN_MASK], 6
    jne .mode_next
    cmp byte [es:VBE_MODE_BUF + MIB_BLUE_MASK], 5
    jne .mode_next
    jmp .bpp_ok
.bpp32:
    ; 8-8-8; the 4th byte is unused padding, which is what fb.c writes to.
    cmp byte [es:VBE_MODE_BUF + MIB_RED_MASK], 8
    jne .mode_next
    cmp byte [es:VBE_MODE_BUF + MIB_GREEN_MASK], 8
    jne .mode_next
    cmp byte [es:VBE_MODE_BUF + MIB_BLUE_MASK], 8
    jne .mode_next
.bpp_ok:
    ; require: resolution matches the current preferred (want_w/want_h)
    mov ax, [es:VBE_MODE_BUF + MIB_WIDTH]
    cmp ax, [vbe_want_w]
    jne .mode_next
    mov ax, [es:VBE_MODE_BUF + MIB_HEIGHT]
    cmp ax, [vbe_want_h]
    jne .mode_next

    ; --- MATCH. -------------------------------------------------------------
    ; Print the chosen mode NOW, while still in text mode: once the graphics
    ; mode is set below, text output to 0xB8000 is no longer displayed, so a
    ; status line printed afterward would be invisible. Geometry comes from the
    ; mode-info block (es=0 still selects it; the print helpers don't touch es).
    mov si, msg_vbe_ok
    call print_string
    mov ax, [es:VBE_MODE_BUF + MIB_WIDTH]
    call print_dec_ax
    mov si, msg_vbe_x
    call print_string
    mov ax, [es:VBE_MODE_BUF + MIB_HEIGHT]
    call print_dec_ax
    mov si, msg_vbe_x
    call print_string
    xor ax, ax                   ; report the bpp actually chosen, not a guess
    mov al, [es:VBE_MODE_BUF + MIB_BPP]
    call print_dec_ax
    mov si, msg_vbe_bpp
    call print_string
    call print_newline

    ; set this mode with the linear-framebuffer bit
    mov bx, [vbe_cur_mode]
    or  bx, SET_LFB_BIT
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .fail                    ; mode-set rejected -> stay text mode (still visible)

    ; --- 4. persist the info struct the kernel reads (es=0) ------------------
    xor ax, ax
    mov es, ax                   ; re-assert es=0 (4F02 may clobber es)
    mov ax, [es:VBE_MODE_BUF + MIB_WIDTH]
    mov [es:VBE_WIDTH], ax
    mov ax, [es:VBE_MODE_BUF + MIB_HEIGHT]
    mov [es:VBE_HEIGHT], ax
    mov al, [es:VBE_MODE_BUF + MIB_BPP]
    mov [es:VBE_BPP], al
    ; pitch: prefer VBE 3.0 LinBytesPerScanLine; fall back to BytesPerScanLine
    mov ax, [es:VBE_MODE_BUF + MIB_LIN_PITCH]
    test ax, ax
    jnz .pitch_ok
    mov ax, [es:VBE_MODE_BUF + MIB_PITCH]
.pitch_ok:
    mov [es:VBE_PITCH], ax
    mov eax, [es:VBE_MODE_BUF + MIB_PHYSBASE]
    mov [es:VBE_FB], eax
    ; mark valid LAST, after every field is written
    mov byte [es:VBE_VALID], 1
    jmp .ret

.pref_advance:
    add si, 4                    ; next (width,height) pair
    jmp .pref_next

.none:
    mov si, msg_vbe_none         ; VBE present, no listed mode offered
    call print_string
    call print_newline
    jmp .ret                     ; VBE_VALID already 0

.fail:
    xor ax, ax
    mov es, ax
    mov byte [es:VBE_VALID], 0
    mov si, msg_vbe_fail
    call print_string
    call print_newline
    jmp .ret
%endif

.ret:
    pop fs
    pop es
    popa
    ret

; ----------------------------------------------------------------------------
; Preference table: ordered (width, height) pairs, all 16bpp 5-6-5 LFB.
; First entry the BIOS actually offers wins. Terminated by 0,0.
; Reorder to change the effective preference; add rows to support more.
; The widescreen / large modes below the fallback chain are present but only
; reachable by moving them up (or via a future runtime picker).
; ----------------------------------------------------------------------------
vbe_pref_table:
    dw 1024, 768        ; XGA   - readable 128x48 text grid, near-universal
    dw  800, 600        ; SVGA
    dw  640, 480        ; VGA   - universal fallback
    dw  640, 350        ; EGA   - rarely an LFB 16bpp mode; falls through if absent
    dw  320, 200        ; CGA-era
    ; --- latent (reorder up to prefer): widescreen / large ---
    dw 1152, 864        ; XGA+
    dw 1280, 720        ; WXGA 16:9
    dw 1280, 768        ; WXGA 15:9
    dw 1280, 800        ; WXGA 16:10
    dw 1280,1024        ; SXGA
    dw 1400,1050        ; SXGA+
    dw 1440, 900        ; WXGA+
    dw 1600, 900        ; HD+
    dw 1600,1200        ; UXGA
    dw 1680,1050        ; WSXGA+
    dw 1920,1080        ; FHD / 1080p
    dw 0, 0             ; terminator

; scratch (real-mode data)
vbe_want_w      dw 0
vbe_want_h      dw 0
vbe_cur_mode    dw 0
vbe_list_seg    dw 0
vbe_list_off    dw 0

; status strings
msg_vbe_ok    db '[BOOT] VBE LFB ', 0
msg_vbe_x     db 'x', 0
msg_vbe_bpp   db 'bpp', 0
msg_vbe_none  db '[BOOT] VBE: no matching mode - text mode', 0
msg_vbe_fail  db '[BOOT] VBE: unavailable - text mode', 0
msg_vbe_off   db '[BOOT] VBE disabled (CXK_ENABLE_FB=0)', 0