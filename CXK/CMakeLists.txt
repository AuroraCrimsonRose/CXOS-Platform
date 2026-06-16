cmake_minimum_required(VERSION 3.20)
project(CXK ASM_NASM)
enable_language(ASM_NASM)

# -------------------------------------------------
# TOOLCHAIN
# -------------------------------------------------
set(CMAKE_ASM_NASM_COMPILER nasm)

set(CROSS_PREFIX "C:/cross/i686-elf/bin")
set(CMAKE_C_COMPILER "${CROSS_PREFIX}/i686-elf-gcc.exe")
set(CMAKE_OBJCOPY "${CROSS_PREFIX}/i686-elf-objcopy.exe")
set(CMAKE_LD "${CROSS_PREFIX}/i686-elf-ld.exe")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -------------------------------------------------
# PATHS
# -------------------------------------------------
set(BUILD_DIR   ${CMAKE_BINARY_DIR})
set(SRC_DIR     ${CMAKE_SOURCE_DIR})

# -------------------------------------------------
# C COMPILE FLAGS (shared by all kernel C files)
#   -I paths let bare includes (#include "idt.h") resolve from subfolders
# -------------------------------------------------
set(SSEFLAGS -msse -msse2 -mfpmath=sse)
set(KCFLAGS -m32 -ffreestanding -fno-pic -fno-stack-protector -Wall -Wextra
    -I${SRC_DIR}/kernel
    -I${SRC_DIR}/kernel/cpu
    -I${SRC_DIR}/kernel/cpu/int
    -I${SRC_DIR}/kernel/drivers/bus
    -I${SRC_DIR}/kernel/drivers/storage
    -I${SRC_DIR}/kernel/drivers/usb
    -I${SRC_DIR}/kernel/drivers/net
    -I${SRC_DIR}/kernel/drivers/video
    -I${SRC_DIR}/kernel/drivers/input
    -I${SRC_DIR}/kernel/drivers/char
    -I${SRC_DIR}/kernel/drivers/power
    -I${SRC_DIR}/kernel/lib
    -I${SRC_DIR}/kernel/shell
    -I${SRC_DIR}/kernel/memman
    -I${SRC_DIR}/kernel/filesys
    -I${SRC_DIR}/kernel/data/fonts)

set(BOOT_SRC    ${SRC_DIR}/boot/boot.asm)
set(STAGE2_SRC  ${SRC_DIR}/boot/stage2.asm)
set(KERNEL_SRC  ${SRC_DIR}/kernel/kernel.asm)
set(KERNEL_C    ${SRC_DIR}/kernel/kernel.c)
set(INIT_C      ${SRC_DIR}/kernel/init.c)

# --- kernel subsystem sources ---
set(IDT_ASM     ${SRC_DIR}/kernel/cpu/int/idt.asm)
set(GDT_C       ${SRC_DIR}/kernel/cpu/gdt.c)
set(GDT_ASM     ${SRC_DIR}/kernel/cpu/gdt_flush.asm)
set(UM_C        ${SRC_DIR}/kernel/cpu/usermode.c)
set(UM_ASM      ${SRC_DIR}/kernel/cpu/usermode.asm)
set(IDT_C       ${SRC_DIR}/kernel/cpu/int/idt.c)
set(PIC_C       ${SRC_DIR}/kernel/cpu/int/pic.c)
set(VGA_C       ${SRC_DIR}/kernel/drivers/video/vga.c)
set(ATA_C       ${SRC_DIR}/kernel/drivers/storage/ata.c)
set(RTC_C       ${SRC_DIR}/kernel/drivers/char/rtc.c)
set(FB_C        ${SRC_DIR}/kernel/drivers/video/fb.c)
set(PCI_C       ${SRC_DIR}/kernel/drivers/bus/pci.c)
set(AHCI_C      ${SRC_DIR}/kernel/drivers/storage/ahci.c)
set(DISK_C      ${SRC_DIR}/kernel/drivers/storage/disk.c)
set(ACPI_C      ${SRC_DIR}/kernel/drivers/power/acpi.c)
set(OHCI_C      ${SRC_DIR}/kernel/drivers/usb/ohci.c)
set(E1000_C     ${SRC_DIR}/kernel/drivers/net/e1000.c)
set(NETIF_C     ${SRC_DIR}/kernel/drivers/net/netif.c)
set(ARP_C       ${SRC_DIR}/kernel/drivers/net/arp.c)
set(IP_C        ${SRC_DIR}/kernel/drivers/net/ip.c)
set(ICMP_C      ${SRC_DIR}/kernel/drivers/net/icmp.c)
set(FPU_C       ${SRC_DIR}/kernel/cpu/fpu.c)
set(KMATH_C     ${SRC_DIR}/kernel/lib/kmath.c)
set(DEMO_C      ${SRC_DIR}/kernel/shell/demo.c)
set(FONT_C      ${SRC_DIR}/kernel/data/fonts/default.c)
set(TIMER_C     ${SRC_DIR}/kernel/drivers/char/timer.c)
set(KBD_C       ${SRC_DIR}/kernel/drivers/input/keyboard.c)
set(CONSOLE_C   ${SRC_DIR}/kernel/drivers/char/console.c)
set(STRING_C    ${SRC_DIR}/kernel/lib/string.c)
set(SHELL_C     ${SRC_DIR}/kernel/shell/shell.c)
set(COMMANDS_C  ${SRC_DIR}/kernel/shell/commands.c)
set(POWER_C     ${SRC_DIR}/kernel/drivers/power/power.c)
set(CXFS_C      ${SRC_DIR}/kernel/filesys/cxfs.c)
set(PMM_C       ${SRC_DIR}/kernel/memman/pmm.c)
set(HEAP_C      ${SRC_DIR}/kernel/memman/heap.c)
set(PAGING_C    ${SRC_DIR}/kernel/memman/paging.c)

set(BOOT_BIN    ${BUILD_DIR}/boot.bin)
set(STAGE2_BIN  ${BUILD_DIR}/stage2.bin)

set(KERNEL_O    ${BUILD_DIR}/kernel.o)
set(KERNEL_CO   ${BUILD_DIR}/kernel_c.o)
set(INIT_CO     ${BUILD_DIR}/init_c.o)

# --- subsystem objects ---
set(IDT_ASM_O   ${BUILD_DIR}/idt_asm.o)
set(GDT_CO      ${BUILD_DIR}/gdt_c.o)
set(GDT_ASM_O   ${BUILD_DIR}/gdt_asm.o)
set(UM_CO       ${BUILD_DIR}/usermode_c.o)
set(UM_ASM_O    ${BUILD_DIR}/usermode_asm.o)
set(IDT_CO      ${BUILD_DIR}/idt_c.o)
set(PIC_CO      ${BUILD_DIR}/pic_c.o)
set(VGA_CO      ${BUILD_DIR}/vga_c.o)
set(ATA_CO      ${BUILD_DIR}/ata_c.o)
set(RTC_CO      ${BUILD_DIR}/rtc_c.o)
set(FB_CO       ${BUILD_DIR}/fb_c.o)
set(PCI_CO      ${BUILD_DIR}/pci_c.o)
set(AHCI_CO     ${BUILD_DIR}/ahci_c.o)
set(DISK_CO     ${BUILD_DIR}/disk_c.o)
set(ACPI_CO     ${BUILD_DIR}/acpi_c.o)
set(OHCI_CO     ${BUILD_DIR}/ohci_c.o)
set(E1000_CO    ${BUILD_DIR}/e1000_c.o)
set(NETIF_CO    ${BUILD_DIR}/netif_c.o)
set(ARP_CO      ${BUILD_DIR}/arp_c.o)
set(IP_CO       ${BUILD_DIR}/ip_c.o)
set(ICMP_CO     ${BUILD_DIR}/icmp_c.o)
set(FPU_CO      ${BUILD_DIR}/fpu_c.o)
set(KMATH_CO    ${BUILD_DIR}/kmath_c.o)
set(DEMO_CO     ${BUILD_DIR}/demo_c.o)
set(FONT_CO     ${BUILD_DIR}/font_c.o)
set(TIMER_CO    ${BUILD_DIR}/timer_c.o)
set(KBD_CO      ${BUILD_DIR}/keyboard_c.o)
set(CONSOLE_CO  ${BUILD_DIR}/console_c.o)
set(STRING_CO   ${BUILD_DIR}/string_c.o)
set(SHELL_CO    ${BUILD_DIR}/shell_c.o)
set(COMMANDS_CO ${BUILD_DIR}/commands_c.o)
set(POWER_CO    ${BUILD_DIR}/power_c.o)
set(CXFS_CO     ${BUILD_DIR}/cxfs_c.o)
set(PMM_CO      ${BUILD_DIR}/pmm_c.o)
set(HEAP_CO     ${BUILD_DIR}/heap_c.o)
set(PAGING_CO   ${BUILD_DIR}/paging_c.o)

set(KERNEL_ELF  ${BUILD_DIR}/kernel.elf)
set(KERNEL_BIN  ${BUILD_DIR}/kernel.bin)

# kernel.o MUST be first (kernel_entry at 0x10000)
set(KERNEL_OBJS ${KERNEL_O} ${KERNEL_CO} ${INIT_CO} ${IDT_ASM_O} ${GDT_ASM_O} ${GDT_CO} ${UM_ASM_O} ${UM_CO} ${IDT_CO} ${PIC_CO} ${VGA_CO} ${TIMER_CO} ${KBD_CO} ${CONSOLE_CO} ${STRING_CO} ${SHELL_CO} ${COMMANDS_CO} ${POWER_CO} ${PMM_CO} ${HEAP_CO} ${PAGING_CO} ${ATA_CO} ${RTC_CO} ${FB_CO} ${PCI_CO} ${AHCI_CO} ${DISK_CO} ${ACPI_CO} ${OHCI_CO} ${E1000_CO} ${NETIF_CO} ${ARP_CO} ${IP_CO} ${ICMP_CO} ${FPU_CO} ${KMATH_CO} ${DEMO_CO} ${FONT_CO} ${CXFS_CO})

set(DIST_DIR    ${SRC_DIR}/dist/CXK_x86_32)
set(DISK_IMG    ${DIST_DIR}/cxk_x86_32.img)
set(FS_IMG      ${DIST_DIR}/cxk_filesystem.img)
set(USB_IMG     ${DIST_DIR}/cxk_usb.img)
set(PAD_SCRIPT  ${SRC_DIR}/tools/pad_disk.ps1)
set(MKFS_SCRIPT ${SRC_DIR}/tools/make_fs_img.ps1)

# -------------------------------------------------
# STAGE2 DEPS
# -------------------------------------------------
set(STAGE2_DEPS
    ${SRC_DIR}/boot/stage2.asm
    ${SRC_DIR}/boot/print.asm
    ${SRC_DIR}/boot/error.asm
    ${SRC_DIR}/boot/a20.asm
    ${SRC_DIR}/boot/mem.asm
    ${SRC_DIR}/boot/gdt.asm
    ${SRC_DIR}/boot/kernel_load.asm
    ${SRC_DIR}/boot/pmode.asm
)

# -------------------------------------------------
# BOOTLOADER
# -------------------------------------------------
add_custom_command(
    OUTPUT ${BOOT_BIN}
    COMMAND nasm -f bin ${BOOT_SRC} -o ${BOOT_BIN}
    DEPENDS ${BOOT_SRC}
)

# -------------------------------------------------
# STAGE2
# -------------------------------------------------
add_custom_command(
    OUTPUT ${STAGE2_BIN}
    COMMAND nasm -I${SRC_DIR}/boot -f bin ${STAGE2_SRC} -o ${STAGE2_BIN}
    DEPENDS ${STAGE2_DEPS}
)

# -------------------------------------------------
# KERNEL ASM (entry)
# -------------------------------------------------
add_custom_command(
    OUTPUT ${KERNEL_O}
    COMMAND nasm -f elf32 ${KERNEL_SRC} -o ${KERNEL_O}
    DEPENDS ${KERNEL_SRC}
)

# -------------------------------------------------
# IDT ASM (interrupt stubs)
# -------------------------------------------------
add_custom_command(
    OUTPUT ${IDT_ASM_O}
    COMMAND nasm -f elf32 ${IDT_ASM} -o ${IDT_ASM_O}
    DEPENDS ${IDT_ASM}
)

add_custom_command(
    OUTPUT ${GDT_ASM_O}
    COMMAND nasm -f elf32 ${GDT_ASM} -o ${GDT_ASM_O}
    DEPENDS ${GDT_ASM}
)

add_custom_command(
    OUTPUT ${GDT_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${GDT_C} -o ${GDT_CO}
    DEPENDS ${GDT_C} ${SRC_DIR}/kernel/cpu/gdt.h
)

add_custom_command(
    OUTPUT ${UM_ASM_O}
    COMMAND nasm -f elf32 ${UM_ASM} -o ${UM_ASM_O}
    DEPENDS ${UM_ASM}
)

add_custom_command(
    OUTPUT ${UM_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${UM_C} -o ${UM_CO}
    DEPENDS ${UM_C} ${SRC_DIR}/kernel/cpu/usermode.h
)

# -------------------------------------------------
# KERNEL C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${KERNEL_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${KERNEL_C} -o ${KERNEL_CO}
    DEPENDS ${KERNEL_C}
)

add_custom_command(
    OUTPUT ${INIT_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${INIT_C} -o ${INIT_CO}
    DEPENDS ${INIT_C} ${SRC_DIR}/kernel/init.h
)

# -------------------------------------------------
# IDT C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${IDT_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${IDT_C} -o ${IDT_CO}
    DEPENDS ${IDT_C} ${SRC_DIR}/kernel/cpu/int/idt.h
)

# -------------------------------------------------
# PIC C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${PIC_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${PIC_C} -o ${PIC_CO}
    DEPENDS ${PIC_C}
)

# -------------------------------------------------
# VGA C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${VGA_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${VGA_C} -o ${VGA_CO}
    DEPENDS ${VGA_C}
)

# -------------------------------------------------
# ATA C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${ATA_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${ATA_C} -o ${ATA_CO}
    DEPENDS ${ATA_C} ${SRC_DIR}/kernel/drivers/storage/ata.h
)

# -------------------------------------------------
# RTC C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${RTC_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${RTC_C} -o ${RTC_CO}
    DEPENDS ${RTC_C} ${SRC_DIR}/kernel/drivers/char/rtc.h
)

# -------------------------------------------------
# Framebuffer C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${FB_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${FB_C} -o ${FB_CO}
    DEPENDS ${FB_C} ${SRC_DIR}/kernel/drivers/video/fb.h
)

add_custom_command(
    OUTPUT ${PCI_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${PCI_C} -o ${PCI_CO}
    DEPENDS ${PCI_C} ${SRC_DIR}/kernel/drivers/bus/pci.h
)

add_custom_command(
    OUTPUT ${AHCI_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${AHCI_C} -o ${AHCI_CO}
    DEPENDS ${AHCI_C} ${SRC_DIR}/kernel/drivers/storage/ahci.h
)

add_custom_command(
    OUTPUT ${DISK_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${DISK_C} -o ${DISK_CO}
    DEPENDS ${DISK_C} ${SRC_DIR}/kernel/drivers/storage/disk.h
)

add_custom_command(
    OUTPUT ${ACPI_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${ACPI_C} -o ${ACPI_CO}
    DEPENDS ${ACPI_C} ${SRC_DIR}/kernel/drivers/power/acpi.h
)

add_custom_command(
    OUTPUT ${OHCI_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${OHCI_C} -o ${OHCI_CO}
    DEPENDS ${OHCI_C} ${SRC_DIR}/kernel/drivers/usb/ohci.h
)

add_custom_command(
    OUTPUT ${E1000_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${E1000_C} -o ${E1000_CO}
    DEPENDS ${E1000_C} ${SRC_DIR}/kernel/drivers/net/e1000.h
)

add_custom_command(
    OUTPUT ${NETIF_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${NETIF_C} -o ${NETIF_CO}
    DEPENDS ${NETIF_C} ${SRC_DIR}/kernel/drivers/net/netif.h
)

add_custom_command(
    OUTPUT ${ARP_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${ARP_C} -o ${ARP_CO}
    DEPENDS ${ARP_C} ${SRC_DIR}/kernel/drivers/net/arp.h
)

add_custom_command(
    OUTPUT ${IP_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${IP_C} -o ${IP_CO}
    DEPENDS ${IP_C} ${SRC_DIR}/kernel/drivers/net/ip.h
)

add_custom_command(
    OUTPUT ${ICMP_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${ICMP_C} -o ${ICMP_CO}
    DEPENDS ${ICMP_C} ${SRC_DIR}/kernel/drivers/net/icmp.h
)

add_custom_command(
    OUTPUT ${FPU_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${FPU_C} -o ${FPU_CO}
    DEPENDS ${FPU_C} ${SRC_DIR}/kernel/cpu/fpu.h
)
add_custom_command(
    OUTPUT ${KMATH_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} ${SSEFLAGS} -c ${KMATH_C} -o ${KMATH_CO}
    DEPENDS ${KMATH_C} ${SRC_DIR}/kernel/lib/kmath.h
)
add_custom_command(
    OUTPUT ${DEMO_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} ${SSEFLAGS} -c ${DEMO_C} -o ${DEMO_CO}
    DEPENDS ${DEMO_C} ${SRC_DIR}/kernel/shell/demo.h
)

# -------------------------------------------------
# Font data C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${FONT_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${FONT_C} -o ${FONT_CO}
    DEPENDS ${FONT_C} ${SRC_DIR}/kernel/data/fonts/default.h
)

# -------------------------------------------------
# CXFS C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${CXFS_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${CXFS_C} -o ${CXFS_CO}
    DEPENDS ${CXFS_C} ${SRC_DIR}/kernel/filesys/cxfs.h
)

# -------------------------------------------------
# TIMER C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${TIMER_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${TIMER_C} -o ${TIMER_CO}
    DEPENDS ${TIMER_C} ${SRC_DIR}/kernel/drivers/char/timer.h
)

# -------------------------------------------------
# KEYBOARD C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${KBD_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${KBD_C} -o ${KBD_CO}
    DEPENDS ${KBD_C} ${SRC_DIR}/kernel/drivers/input/keyboard.h
)

# -------------------------------------------------
# CONSOLE C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${CONSOLE_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${CONSOLE_C} -o ${CONSOLE_CO}
    DEPENDS ${CONSOLE_C} ${SRC_DIR}/kernel/drivers/char/console.h ${SRC_DIR}/kernel/drivers/video/vga.h
)

# -------------------------------------------------
# STRING (lib) C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${STRING_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${STRING_C} -o ${STRING_CO}
    DEPENDS ${STRING_C} ${SRC_DIR}/kernel/lib/string.h
)

# -------------------------------------------------
# SHELL C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${SHELL_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${SHELL_C} -o ${SHELL_CO}
    DEPENDS ${SHELL_C} ${SRC_DIR}/kernel/shell/shell.h
)

# -------------------------------------------------
# COMMANDS C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${COMMANDS_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${COMMANDS_C} -o ${COMMANDS_CO}
    DEPENDS ${COMMANDS_C} ${SRC_DIR}/kernel/shell/commands.h
)

# -------------------------------------------------
# POWER C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${POWER_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${POWER_C} -o ${POWER_CO}
    DEPENDS ${POWER_C} ${SRC_DIR}/kernel/drivers/power/power.h
)

# -------------------------------------------------
# PMM C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${PMM_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${PMM_C} -o ${PMM_CO}
    DEPENDS ${PMM_C} ${SRC_DIR}/kernel/memman/pmm.h
)

# -------------------------------------------------
# HEAP C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${HEAP_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${HEAP_C} -o ${HEAP_CO}
    DEPENDS ${HEAP_C} ${SRC_DIR}/kernel/memman/heap.h
)

# -------------------------------------------------
# PAGING C
# -------------------------------------------------
add_custom_command(
    OUTPUT ${PAGING_CO}
    COMMAND ${CMAKE_C_COMPILER} ${KCFLAGS} -c ${PAGING_C} -o ${PAGING_CO}
    DEPENDS ${PAGING_C} ${SRC_DIR}/kernel/memman/paging.h
)

# -------------------------------------------------
# LINK
# -------------------------------------------------
add_custom_command(
    OUTPUT ${KERNEL_ELF}
    COMMAND ${CMAKE_LD}
        -T ${SRC_DIR}/linker.ld
        -o ${KERNEL_ELF}
        ${KERNEL_OBJS}
        -Map=${BUILD_DIR}/kernel.map
        -cref
    DEPENDS ${KERNEL_OBJS}
)

# -------------------------------------------------
# BIN
# -------------------------------------------------
add_custom_command(
    OUTPUT ${KERNEL_BIN}
    COMMAND ${CMAKE_OBJCOPY}
        -O binary ${KERNEL_ELF} ${KERNEL_BIN}
    DEPENDS ${KERNEL_ELF}
)

# -------------------------------------------------
# DISK IMAGE
# -------------------------------------------------
add_custom_command(
    OUTPUT ${DISK_IMG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}
    COMMAND ${CMAKE_COMMAND} -E echo "Building boot disk image..."
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File ${PAD_SCRIPT}
        -Boot ${BOOT_BIN}
        -Stage2 ${STAGE2_BIN}
        -Kernel ${KERNEL_BIN}
        -Out ${DISK_IMG}
    DEPENDS ${BOOT_BIN} ${STAGE2_BIN} ${KERNEL_BIN}
)

# -------------------------------------------------
# FILESYSTEM IMAGE (empty 16 MB disk, created once if missing)
# -------------------------------------------------
add_custom_command(
    OUTPUT ${FS_IMG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}
    COMMAND ${CMAKE_COMMAND} -E echo "Creating filesystem image (if missing)..."
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File ${MKFS_SCRIPT}
        -Out ${FS_IMG}
        -SizeMB 16
)

# -------------------------------------------------
# USB FILESYSTEM IMAGE (second 16 MB disk, for the virtual USB mass-storage
# device the emulators attach - lets us develop the USB stack against a real
# block device. Created once if missing, like the FS image.)
# -------------------------------------------------
add_custom_command(
    OUTPUT ${USB_IMG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST_DIR}
    COMMAND ${CMAKE_COMMAND} -E echo "Creating USB image (if missing)..."
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File ${MKFS_SCRIPT}
        -Out ${USB_IMG}
        -SizeMB 16
)

# -------------------------------------------------
# TARGETS
# -------------------------------------------------
add_custom_target(bootloader ALL DEPENDS ${BOOT_BIN})
add_custom_target(stage2 ALL DEPENDS ${STAGE2_BIN})
add_custom_target(kernel ALL DEPENDS ${KERNEL_BIN})
add_custom_target(disk_image ALL DEPENDS ${DISK_IMG} ${FS_IMG} ${USB_IMG})

add_dependencies(stage2 bootloader)
add_dependencies(kernel stage2)
add_dependencies(disk_image kernel)