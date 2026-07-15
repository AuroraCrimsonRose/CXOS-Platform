# ============================================================================
#  CXK v5 - CMakeLists.txt
#  Copyright (c) 2026 CATX Systems LLC.  All rights reserved.
#  Author: Aurora Tejeda / CATX Systems LLC
# ============================================================================
#
# v5 build - boot chain + higher-half kernel.
#   stage 1 (boot.asm) -> stage 2 (a20/mem/gdt/pmode + CXEX load) ->
#   kernel (higher-half at 0xC0100000).
#
# Kernel sources are LISTS (KERNEL_C_SRCS / KERNEL_ASM_SRCS): to add a
# subsystem, append its .c/.asm here - the compile+link rules are generated
# automatically. Include dirs are in KERNEL_INCLUDES.

cmake_minimum_required(VERSION 3.15)
project(CXK_v5 ASM_NASM)
enable_language(ASM_NASM)

# ---- repo root ----
# This file lives at <root>/tools/cmake/CMakeLists.txt. Derive the project root
# from its own location so the build works regardless of the working directory
# cmake is invoked from. Everything below uses CXK_ROOT, not CMAKE_SOURCE_DIR.
get_filename_component(CXK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# ---- build-time flags (edit cmake/cxk_flags.cmake, NOT this file) ----
# One source of truth for toggles like CXK_ENABLE_FB. Each NAME=VALUE becomes
# -DNAME=VALUE and is fed to BOTH gcc (kernel C) and nasm (boot + kernel asm),
# so the kernel and bootloader can't disagree.
include(${CXK_ROOT}/tools/cmake/cxk_flags.cmake)
set(CXK_DEFS "")
foreach(f ${CXK_FLAGS})
    list(APPEND CXK_DEFS -D${f})
endforeach()

# ---- toolchain ----
set(CROSS_PREFIX "C:/cross/i686-elf/bin")
set(CMAKE_ASM_NASM_COMPILER nasm)
set(CMAKE_OBJCOPY "${CROSS_PREFIX}/i686-elf-objcopy.exe")
set(CMAKE_LD      "${CROSS_PREFIX}/i686-elf-ld.exe")
set(KCC           "${CROSS_PREFIX}/i686-elf-gcc.exe")

# ---- paths ----
set(SRC_DIR   ${CXK_ROOT})
set(BUILD_DIR ${CMAKE_BINARY_DIR})
set(IMAGES_DIR  ${CXK_ROOT}/dist/CXK_x86_32/images)
set(DIST_DIR    ${CXK_ROOT}/dist/CXK_x86_32/packages)
set(PACKAGES_DIR ${DIST_DIR})

# ---- kernel include directories (add new subsystem dirs here) ----
set(KERNEL_INCLUDES
    -I${SRC_DIR}/abi
    -I${SRC_DIR}/kernel
    -I${SRC_DIR}/kernel/lib/time
    -I${SRC_DIR}/kernel/lib/audio
    -I${SRC_DIR}/kernel/lib/string
    -I${SRC_DIR}/kernel/lib/math
    -I${SRC_DIR}/kernel/lib/crypto
    -I${SRC_DIR}/kernel/lib/mem
    -I${SRC_DIR}/kernel/lib/gfx
    -I${SRC_DIR}/kernel/lib/format
    -I${SRC_DIR}/kernel/drivers/storage
    -I${SRC_DIR}/kernel/drivers/storage/partition
    -I${SRC_DIR}/kernel/cpu
    -I${SRC_DIR}/kernel/cpu/int
    -I${SRC_DIR}/kernel/memman
    -I${SRC_DIR}/kernel/drivers/char
    -I${SRC_DIR}/kernel/drivers/video
    -I${SRC_DIR}/kernel/drivers/bus
    -I${SRC_DIR}/kernel/drivers/storage/hardware
    -I${SRC_DIR}/kernel/drivers/storage/filesys
)

# freestanding kernel C flags
set(KCFLAGS -ffreestanding -fno-pic -fno-stack-protector -m32
            -Wall -Wextra ${CXK_DEFS} ${KERNEL_INCLUDES} -c)

# ---- KERNEL C SOURCES (append new subsystems here) ----
set(KERNEL_C_SRCS
    ${SRC_DIR}/kernel/kmain.c
    ${SRC_DIR}/kernel/ktest.c
    ${SRC_DIR}/kernel/cpu/gdt.c
    ${SRC_DIR}/kernel/cpu/int/idt.c
    ${SRC_DIR}/kernel/cpu/int/pic.c
    ${SRC_DIR}/kernel/memman/pmm.c
    ${SRC_DIR}/kernel/memman/paging.c
    ${SRC_DIR}/kernel/memman/addr_space.c
    ${SRC_DIR}/kernel/memman/heap.c
    ${SRC_DIR}/kernel/lib/time/datetime.c
    ${SRC_DIR}/kernel/lib/string/string.c
    ${SRC_DIR}/kernel/lib/mem/bitmap.c
    ${SRC_DIR}/kernel/lib/string/format.c
    ${SRC_DIR}/kernel/lib/string/logging.c
    ${SRC_DIR}/kernel/lib/string/syslog.c
    ${SRC_DIR}/kernel/cpu/sched.c
    ${SRC_DIR}/kernel/cpu/uid.c
    ${SRC_DIR}/kernel/drivers/char/timer.c
    ${SRC_DIR}/kernel/drivers/char/rtc.c
    ${SRC_DIR}/kernel/cpu/usermode.c
    ${SRC_DIR}/kernel/cpu/ipc.c
    ${SRC_DIR}/kernel/cpu/spawn.c
    ${SRC_DIR}/kernel/cpu/handle.c
    ${SRC_DIR}/kernel/cpu/exec.c
    ${SRC_DIR}/kernel/cpu/launch.c
    ${SRC_DIR}/kernel/drivers/char/console.c
    ${SRC_DIR}/kernel/drivers/video/vga.c
    ${SRC_DIR}/kernel/drivers/video/fb.c
    ${SRC_DIR}/kernel/lib/audio/speaker.c
    ${SRC_DIR}/kernel/lib/gfx/color.c
    ${SRC_DIR}/kernel/lib/gfx/resolution.c
    ${SRC_DIR}/kernel/lib/gfx/font.c
    ${SRC_DIR}/kernel/lib/gfx/klogo.c
    ${SRC_DIR}/kernel/drivers/bus/pci.c
    ${SRC_DIR}/kernel/drivers/storage/hardware/ata.c
    ${SRC_DIR}/kernel/drivers/storage/hardware/ahci.c
    ${SRC_DIR}/kernel/drivers/storage/hardware/disk.c
    ${SRC_DIR}/kernel/drivers/storage/filesys/cxfs.c
    ${SRC_DIR}/kernel/drivers/storage/partition/partition.c
    ${SRC_DIR}/kernel/drivers/storage/install.c
    ${SRC_DIR}/kernel/lib/crypto/sha256.c
    ${SRC_DIR}/kernel/lib/crypto/bignum.c
    ${SRC_DIR}/kernel/lib/crypto/rsa.c
    ${SRC_DIR}/kernel/lib/format/cxex.c
    ${SRC_DIR}/kernel/lib/format/cxex_verify.c
    ${SRC_DIR}/kernel/lib/format/trusted_key.c
    ${SRC_DIR}/kernel/lib/format/cxex_load.c
    ${SRC_DIR}/kernel/lib/format/cxex_loadk.c
)

# ---- KERNEL ASM SOURCES (elf32 objects; append new ones here) ----
set(KERNEL_ASM_SRCS
    ${SRC_DIR}/kernel/kernel.asm
    ${SRC_DIR}/kernel/cpu/gdt_flush.asm
    ${SRC_DIR}/kernel/cpu/int/idt.asm
    ${SRC_DIR}/kernel/cpu/switch.asm
    ${SRC_DIR}/kernel/cpu/usermode.asm
)

# ---- linker script ----
set(LINKER ${SRC_DIR}/linker.ld)

# ---- boot sources ----
set(BOOT_SRC   ${SRC_DIR}/boot/boot.asm)
set(STAGE2_SRC ${SRC_DIR}/boot/stage2.asm)

# ---- outputs ----
set(BOOT_BIN    ${BUILD_DIR}/boot.bin)
set(STAGE2_BIN  ${BUILD_DIR}/stage2.bin)
set(KERNEL_ELF  ${BUILD_DIR}/kernel.elf)
set(KERNEL_XKEX ${DIST_DIR}/kernel.xkex)
set(DISK_IMG    ${IMAGES_DIR}/cxk_x86_32.img)

# the system executive (.xoex): os/executive/executive.c -> ELF -> CXEX (type os)
set(EXEC_C    ${SRC_DIR}/os/executive/executive.c)
set(EXEC_LD   ${SRC_DIR}/os/executive/executive.ld)
set(EXEC_ELF  ${BUILD_DIR}/executive.elf)
set(EXEC_XOEX ${PACKAGES_DIR}/executive.xoex)

include(${CXK_ROOT}/tools/cmake/cxk_toolchain.cmake)

# the unified XBPT disk (boot + stage + system on one disk)
set(CXK_DISK  ${IMAGES_DIR}/cxk_disk.img)

# ---- generate compile rules for each kernel C source ----
set(KERNEL_OBJS "")
foreach(src ${KERNEL_C_SRCS})
    get_filename_component(name ${src} NAME_WE)
    set(obj ${BUILD_DIR}/k_${name}.o)
    add_custom_command(
        OUTPUT ${obj}
        COMMAND ${KCC} ${KCFLAGS} ${src} -o ${obj}
        DEPENDS ${src}
        COMMENT "CC  ${name}.c"
    )
    list(APPEND KERNEL_OBJS ${obj})
endforeach()

# ---- generate assemble rules for each kernel ASM source ----
foreach(src ${KERNEL_ASM_SRCS})
    get_filename_component(name ${src} NAME_WE)
    set(obj ${BUILD_DIR}/ka_${name}.o)
    add_custom_command(
        OUTPUT ${obj}
        COMMAND ${CMAKE_ASM_NASM_COMPILER} -f elf32 ${src} -o ${obj}
        DEPENDS ${src}
        COMMENT "ASM ${name}.asm"
    )
    list(APPEND KERNEL_OBJS ${obj})
endforeach()

# ---- stage 1 (flat binary) ----
add_custom_command(
    OUTPUT ${BOOT_BIN}
    COMMAND ${CMAKE_ASM_NASM_COMPILER} -f bin ${BOOT_SRC} -o ${BOOT_BIN}
    DEPENDS ${BOOT_SRC}
    COMMENT "Assembling stage 1 (boot.asm)"
)

# ---- stage 2 (flat binary; -I boot for %includes) ----
add_custom_command(
    OUTPUT ${STAGE2_BIN}
    COMMAND ${CMAKE_ASM_NASM_COMPILER} ${CXK_DEFS} -I${SRC_DIR}/boot -f bin ${STAGE2_SRC} -o ${STAGE2_BIN}
    DEPENDS ${STAGE2_SRC}
        ${SRC_DIR}/boot/a20.asm ${SRC_DIR}/boot/mem.asm
        ${SRC_DIR}/boot/gdt.asm ${SRC_DIR}/boot/pmode.asm
        ${SRC_DIR}/boot/cxexload.asm ${SRC_DIR}/boot/vbe.asm
    COMMENT "Assembling stage 2 (stage2.asm + includes)"
)

# ---- link the kernel (higher-half) ----
add_custom_command(
    OUTPUT ${KERNEL_ELF}
    COMMAND ${CMAKE_LD} -T ${LINKER} -o ${KERNEL_ELF} ${KERNEL_OBJS}
    DEPENDS ${KERNEL_OBJS}
    COMMENT "Linking kernel (higher-half)"
)

# ---- kernel.elf -> kernel.xkex (CXEX), signed when -DSIGN=ON ----
add_custom_command(
    OUTPUT ${KERNEL_XKEX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
    COMMAND ${CXK} build ${KERNEL_ELF} ${KERNEL_XKEX} --type kernel
    ${KSIGN}
    DEPENDS ${KERNEL_ELF} ${CXK}
    COMMENT "Converting kernel.elf -> kernel.xkex (CXEX)"
)

# ---- executive.c -> executive.elf (user-half) -> executive.xoex (signed) ----
add_custom_command(
    OUTPUT ${EXEC_ELF}
    COMMAND ${KCC} -ffreestanding -fno-pic -no-pie -fno-stack-protector -nostdlib
            -I${SRC_DIR}/abi
            -Wl,-T,${EXEC_LD} -o ${EXEC_ELF} ${EXEC_C}
    DEPENDS ${EXEC_C} ${EXEC_LD} ${SRC_DIR}/os/executive/app_image.h
    COMMENT "CC+LD executive.c -> executive.elf (user-half .xoex bootstrap)"
)
add_custom_command(
    OUTPUT ${EXEC_XOEX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
    COMMAND ${CXK} build ${EXEC_ELF} ${EXEC_XOEX} --type os
    ${XSIGN}
    DEPENDS ${EXEC_ELF} ${CXK}
    COMMENT "Converting executive.elf -> executive.xoex (CXEX, type os)"
)

# ---- unified disk: stage1 + XBPT + stage2 + BOOT/STAGE/SYSTEM partitions ----
add_custom_command(
    OUTPUT ${CXK_DISK}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${IMAGES_DIR}
    COMMAND ${CXK} image --out ${CXK_DISK} --size-mb 64 --boot-mb 8
            --stage1 ${BOOT_BIN} --stage2 ${STAGE2_BIN} --kernel ${KERNEL_XKEX}
            --stage Boot.xoex=${EXEC_XOEX}
    DEPENDS ${BOOT_BIN} ${STAGE2_BIN} ${KERNEL_XKEX} ${EXEC_XOEX} ${CXK}
    COMMENT "Building unified CXK disk (XBPT: boot + stage + system)"
)
add_custom_target(cxk_disk ALL DEPENDS ${CXK_DISK})

# ---- image: boot + stage2 + kernel ----
add_custom_command(
    OUTPUT ${DISK_IMG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${IMAGES_DIR}
    COMMAND ${CMAKE_COMMAND} -E echo "Building v5 boot image..."
    COMMAND ${CXK} raw-image --out ${DISK_IMG}
        --boot ${BOOT_BIN} --stage2 ${STAGE2_BIN} --kernel ${KERNEL_XKEX}
        --kernel-lba 33
    DEPENDS ${BOOT_BIN} ${STAGE2_BIN} ${KERNEL_XKEX} ${CXK}
    COMMENT "Imaging boot + stage 2 + kernel"
)

add_custom_target(disk_image ALL DEPENDS ${DISK_IMG})