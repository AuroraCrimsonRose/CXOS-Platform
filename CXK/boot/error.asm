; /CXLite/boot/error.asm
; Aurora Tejeda

; ----------------------------------
; error codes
; INT 13h Disk Services
; ----------------------------------
ERR_DISK_SUCCESS        equ 0x00
ERR_DISK_BAD_CMD        equ 0x01
ERR_DISK_NO_MARK        equ 0x02
ERR_DISK_WRITE_PROT     equ 0x03
ERR_DISK_NO_SECTOR      equ 0x04
ERR_DISK_RESET_FAIL     equ 0x05
ERR_DISK_CHANGED        equ 0x06
ERR_DISK_PARAM_FAIL     equ 0x07
ERR_DISK_DMA_OVERRUN    equ 0x08
ERR_DISK_DMA_BOUNDARY   equ 0x09
ERR_DISK_BAD_SECTOR     equ 0x0A
ERR_DISK_BAD_TRACK      equ 0x0B
ERR_DISK_INVALID_MEDIA  equ 0x0C
ERR_DISK_INVALID_FMT    equ 0x0D
ERR_DISK_CTRL_MARK      equ 0x0E
ERR_DISK_DMA_RANGE      equ 0x0F
ERR_DISK_ECC            equ 0x10
ERR_DISK_ECC_CORRECTED  equ 0x11
ERR_DISK_CTRL_FAIL      equ 0x20
ERR_DISK_NO_MEDIA       equ 0x31
ERR_DISK_BAD_CMOS       equ 0x32
ERR_DISK_SEEK_FAIL      equ 0x40
ERR_DISK_TIMEOUT        equ 0x80
ERR_DISK_NOT_READY      equ 0xAA
ERR_DISK_NOT_LOCKED     equ 0xB0
ERR_DISK_LOCKED         equ 0xB1
ERR_DISK_NOT_REMOVABLE  equ 0xB2
ERR_DISK_IN_USE         equ 0xB3
ERR_DISK_LOCK_EXCEEDED  equ 0xB4
ERR_DISK_EJECT_FAIL     equ 0xB5
ERR_DISK_UNDEFINED      equ 0xBB
ERR_DISK_WRITE_FAULT    equ 0xCC
ERR_DISK_STATUS         equ 0xE0
ERR_DISK_SENSE_FAIL     equ 0xFF

; ----------------------------------
; INT 15h Memory / Misc Services
; ----------------------------------
ERR_MEM_SUCCESS         equ 0x00
ERR_MEM_KBD_FULL        equ 0x01
ERR_MEM_INTERFACE       equ 0x03
ERR_MEM_A20_IN_USE      equ 0x06
ERR_MEM_INVALID_CMD     equ 0x80
ERR_MEM_NOT_SUPPORTED   equ 0x82
ERR_MEM_IN_PROGRESS     equ 0x83
ERR_MEM_NO_E820         equ 0x86
ERR_MEM_A20_FAIL        equ 0x87

; ----------------------------------
; INT 10h Video Services
; ----------------------------------
ERR_VID_SUCCESS         equ 0x00
ERR_VID_INVALID_MODE    equ 0x01
ERR_VID_NOT_SUPPORTED   equ 0x03
ERR_VID_NO_PAGE         equ 0x05
ERR_VID_SCAN_RANGE      equ 0x06

; ----------------------------------
; INT 16h Keyboard Services
; ----------------------------------
ERR_KBD_SUCCESS         equ 0x00
ERR_KBD_EMPTY           equ 0x01

; ----------------------------------
; panic
; prints null terminated string at DS:SI
; then halts — no return
; ----------------------------------
panic:
    call print_string
    call print_newline
.hang:
    hlt
    jmp .hang

; ----------------------------------
; print_error_code
; in: AL = error byte
; prints '0x' followed by 2 hex digits
; ----------------------------------
print_error_code:
    push ax
    push bx

    ; print '0x'
    mov al, '0'
    call print_char
    mov al, 'x'
    call print_char

    pop bx
    pop ax
    push ax
    push bx

    ; high nibble
    mov bl, al
    shr al, 4
    cmp al, 9
    jbe .high_digit
    add al, 'A' - 10
    jmp .print_high
.high_digit:
    add al, '0'
.print_high:
    call print_char

    ; low nibble
    mov al, bl
    and al, 0x0F
    cmp al, 9
    jbe .low_digit
    add al, 'A' - 10
    jmp .print_low
.low_digit:
    add al, '0'
.print_low:
    call print_char

    pop bx
    pop ax
    ret