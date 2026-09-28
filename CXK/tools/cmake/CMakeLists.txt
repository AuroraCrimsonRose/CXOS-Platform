# ============================================================================
#  CXK v5 - CMakeLists.txt
#  Copyright (c) 2026 CATX Systems LLC.  All rights reserved.
#  Author: Aurora Tejeda / CATX Systems LLC
# ============================================================================
#
# v5 build - boot chain + higher-half kernel + X apps.
#   stage 1 (boot.asm) -> stage 2 (a20/mem/gdt/pmode + CXEX load) ->
#   kernel (higher-half at 0xC0100000) -> executive (.xoex) -> shell (.xcex)
#
# Kernel sources are LISTS (KERNEL_C_SRCS / KERNEL_ASM_SRCS): to add a
# subsystem, append its .c/.asm here - the compile+link rules are generated
# automatically. Include dirs are in KERNEL_INCLUDES.
#
# X apps build automatically: edit os/apps/shell.xfxn and a plain build
# recompiles it, repackages the .xcex, regenerates app_image.h, relinks the
# executive, and reimages the disk.

cmake_minimum_required(VERSION 3.15)
project(CXK_v5 ASM_NASM)
enable_language(ASM_NASM)

# ---- build-time flags (edit cmake/cxk_flags.cmake, NOT this file) ----
# One source of truth for toggles like CXK_ENABLE_FB. Each NAME=VALUE becomes
# -DNAME=VALUE and is fed to BOTH gcc (kernel C) and nasm (boot + kernel asm),
# so the kernel and bootloader can't disagree.
include(${CMAKE_SOURCE_DIR}/cxk_flags.cmake)
set(CXK_DEFS "")
foreach(f ${CXK_FLAGS})
    list(APPEND CXK_DEFS -D${f})
endforeach()

# ---- toolchain ----
# Defaults are the Windows cross-toolchain layout build.bat expects. Each is
# overridable with -D so the kernel can also be built where the toolchain lives
# elsewhere or has no .exe suffix (a Linux/macOS host, CI, a different install
# path). Overriding nothing reproduces the previous behaviour exactly.
if(NOT DEFINED CROSS_PREFIX)
    set(CROSS_PREFIX "C:/cross/i686-elf/bin")
endif()
if(NOT DEFINED CROSS_SUFFIX)
    set(CROSS_SUFFIX ".exe")
endif()
if(NOT DEFINED CMAKE_ASM_NASM_COMPILER)
    set(CMAKE_ASM_NASM_COMPILER nasm)
endif()
if(NOT DEFINED CMAKE_OBJCOPY)
    set(CMAKE_OBJCOPY "${CROSS_PREFIX}/i686-elf-objcopy${CROSS_SUFFIX}")
endif()
if(NOT DEFINED CMAKE_LD)
    set(CMAKE_LD      "${CROSS_PREFIX}/i686-elf-ld${CROSS_SUFFIX}")
endif()
if(NOT DEFINED KCC)
    set(KCC           "${CROSS_PREFIX}/i686-elf-gcc${CROSS_SUFFIX}")
endif()

# ---- paths ----
# Step out of tools/cmake to point to the actual CXK root directory
get_filename_component(SRC_DIR "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
set(BUILD_DIR ${CMAKE_BINARY_DIR})
set(IMAGES_DIR   ${SRC_DIR}/dist/CXK_x86_32/images)
set(DIST_DIR     ${SRC_DIR}/dist/CXK_x86_32/packages)
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
    -I${SRC_DIR}/kernel/drivers/usb
    -I${SRC_DIR}/kernel/drivers/power
    -I${SRC_DIR}/kernel/drivers/input
    -I${SRC_DIR}/kernel/drivers/video
    -I${SRC_DIR}/kernel/drivers/input
    -I${SRC_DIR}/kernel/drivers/power
    -I${SRC_DIR}/kernel/drivers/bus
    -I${SRC_DIR}/kernel/drivers/storage/hardware
    -I${SRC_DIR}/kernel/drivers/storage/filesys
    -I${SRC_DIR}/kernel/drivers/net
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
    ${SRC_DIR}/kernel/cpu/int/apic.c
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
    ${SRC_DIR}/kernel/cpu/sysfile.c
    ${SRC_DIR}/kernel/cpu/handle.c
    ${SRC_DIR}/kernel/cpu/exec.c
    ${SRC_DIR}/kernel/cpu/launch.c
    ${SRC_DIR}/kernel/drivers/char/console.c
    ${SRC_DIR}/kernel/drivers/video/vga.c
    ${SRC_DIR}/kernel/drivers/video/fb.c
    ${SRC_DIR}/kernel/drivers/input/keyboard.c
    ${SRC_DIR}/kernel/drivers/input/mouse.c
    ${SRC_DIR}/kernel/drivers/power/power.c
    ${SRC_DIR}/kernel/drivers/power/acpi.c
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
    ${SRC_DIR}/kernel/drivers/net/arp.c
    ${SRC_DIR}/kernel/drivers/net/e1000.c
    ${SRC_DIR}/kernel/drivers/net/icmp.c
    ${SRC_DIR}/kernel/drivers/net/ip.c
    ${SRC_DIR}/kernel/drivers/net/net_syscall.c
    ${SRC_DIR}/kernel/drivers/net/netif.c
    ${SRC_DIR}/kernel/drivers/net/rtl8169.c
    ${SRC_DIR}/kernel/drivers/usb/usb.c
    ${SRC_DIR}/kernel/drivers/usb/ehci.c
    ${SRC_DIR}/kernel/drivers/usb/xhci.c
    ${SRC_DIR}/kernel/drivers/usb/ohci.c
    ${SRC_DIR}/kernel/drivers/usb/usb_storage.c
    ${SRC_DIR}/kernel/drivers/usb/usb_hid.c
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

# ---- X apps: .xfxn -> .elf -> .xcex -> embedded C header ----
# APP_BOOT is the startup app: embedded into the executive and spawned at boot.
# To swap shells, point APP_BOOT/APP_BOOT_NAME at a different .xfxn.
set(APP_BOOT      ${SRC_DIR}/os/apps/shell.xfxn)
set(APP_BOOT_NAME shell)

set(APP_ELF   ${BUILD_DIR}/${APP_BOOT_NAME}.elf)
set(APP_XCEX  ${BUILD_DIR}/${APP_BOOT_NAME}.xcex)
set(APP_IMAGE ${SRC_DIR}/os/executive/app_image.h)

# DISK_APP is the opposite of APP_BOOT: not embedded in the executive, but
# staged onto the disk as a file and started with SYS_EXEC_PATH. It has to be
# SIGNED, because the kernel verifies what it loads from disk - an unsigned
# .xcex sitting in /System is refused, which is the property worth having.
set(DISK_APP      ${SRC_DIR}/os/apps/hi.xfxn)
set(DISK_APP_NAME hi)
set(DISK_APP_ELF  ${BUILD_DIR}/${DISK_APP_NAME}.elf)
set(DISK_APP_XCEX ${PACKAGES_DIR}/${DISK_APP_NAME}.xcex)

# svcd, the service supervisor. Staged and signed exactly like DISK_APP - the
# executive starts it with SYS_EXEC_PATH, so the kernel verifies it on the way
# in and an unsigned one is refused. That is the property worth having for the
# process that decides what else gets to run.
set(SVCD_SRC  ${SRC_DIR}/os/apps/svcd.xfxn)
set(SVCD_ELF  ${BUILD_DIR}/svcd.elf)
set(SVCD_XCEX ${PACKAGES_DIR}/svcd.xcex)

# Everything APP_BOOT can pull in with `import`, so editing any of it actually
# rebuilds. This variable was referenced by the compile rule below but never
# defined anywhere, and CMake expands an undefined variable to nothing - so the
# rule effectively had no source dependencies beyond shell.xfxn itself. Editing
# gui.xfxn, gfx.xfxn or cxos.xfxn produced a silently stale shell.elf, with the
# old code still embedded in app_image.h and booted.
#
# The glob is deliberately coarse: it covers both import roots (os/std for the
# -I path, os/apps because shell.xfxn imports cxos.xfxn from its own directory),
# so any app rebuilds when any library changes. Correct dependencies beat
# minimal ones here - the alternative is testing a binary you did not build.
# CONFIGURE_DEPENDS makes CMake re-glob, so a NEW .xfxn is noticed without a
# manual re-configure. If this ever gets slow, the real fix is a depfile emitted
# by `cxk compile` listing the imports it actually resolved.
file(GLOB STD_SRCS CONFIGURE_DEPENDS
     ${SRC_DIR}/os/std/*.xfxn
     ${SRC_DIR}/os/apps/*.xfxn)

# the system executive (.xoex): os/executive/executive.c -> ELF -> CXEX (type os)
set(EXEC_C    ${SRC_DIR}/os/executive/executive.c)
set(EXEC_LD   ${SRC_DIR}/os/executive/executive.ld)
set(EXEC_ELF  ${BUILD_DIR}/executive.elf)
set(EXEC_XOEX ${PACKAGES_DIR}/executive.xoex)

include(${CMAKE_SOURCE_DIR}/cxk_toolchain.cmake)

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

# ---- X app: shell.xfxn -> shell.elf ----
add_custom_command(
    OUTPUT ${APP_ELF}
    COMMAND ${CXK} compile ${APP_BOOT} ${APP_ELF} -I ${SRC_DIR}/os/std
    DEPENDS ${APP_BOOT} ${CXK} ${STD_SRCS}
    COMMENT "X   ${APP_BOOT_NAME}.xfxn -> ELF"
)

# ---- shell.elf -> shell.xcex (CXEX, type user) ----
add_custom_command(
    OUTPUT ${APP_XCEX}
    COMMAND ${CXK} build ${APP_ELF} ${APP_XCEX} --type user
    DEPENDS ${APP_ELF} ${CXK}
    COMMENT "Packaging ${APP_BOOT_NAME}.xcex (CXEX, type user)"
)

# ---- shell.xcex -> app_image.h (byte array the executive embeds) ----
add_custom_command(
    OUTPUT ${APP_IMAGE}
    COMMAND ${CXK} embed ${APP_XCEX} ${APP_IMAGE} ${APP_BOOT_NAME}_xcex
    DEPENDS ${APP_XCEX} ${CXK}
    COMMENT "Embedding ${APP_BOOT_NAME}.xcex -> app_image.h"
)

# ---- disk app: hi.xfxn -> ELF -> signed .xcex (staged, not embedded) ----
add_custom_command(
    OUTPUT ${DISK_APP_ELF}
    COMMAND ${CXK} compile ${DISK_APP} ${DISK_APP_ELF} -I ${SRC_DIR}/os/std
    DEPENDS ${DISK_APP} ${CXK} ${STD_SRCS}
    COMMENT "X   ${DISK_APP_NAME}.xfxn -> ELF"
)
add_custom_command(
    OUTPUT ${DISK_APP_XCEX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
    COMMAND ${CXK} build ${DISK_APP_ELF} ${DISK_APP_XCEX} --type user
    ${ASIGN}
    DEPENDS ${DISK_APP_ELF} ${CXK}
    COMMENT "Packaging ${DISK_APP_NAME}.xcex (CXEX, type user, staged to disk)"
)

# ---- svcd: the service supervisor (staged + signed, like the disk app) ----
add_custom_command(
    OUTPUT ${SVCD_ELF}
    COMMAND ${CXK} compile ${SVCD_SRC} ${SVCD_ELF} -I ${SRC_DIR}/os/std
    DEPENDS ${SVCD_SRC} ${CXK} ${STD_SRCS}
    COMMENT "X   svcd.xfxn -> ELF"
)
add_custom_command(
    OUTPUT ${SVCD_XCEX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
    COMMAND ${CXK} build ${SVCD_ELF} ${SVCD_XCEX} --type user
    ${VSIGN}
    DEPENDS ${SVCD_ELF} ${CXK}
    COMMENT "Packaging svcd.xcex (CXEX, type user, staged to disk)"
)

# ---- executive.c -> executive.elf (user-half) -> executive.xoex (signed) ----
add_custom_command(
    OUTPUT ${EXEC_ELF}
    COMMAND ${KCC} -ffreestanding -fno-pic -no-pie -fno-stack-protector -nostdlib
            -I${SRC_DIR}/abi
            -Wl,-T,${EXEC_LD} -o ${EXEC_ELF} ${EXEC_C}
    DEPENDS ${EXEC_C} ${EXEC_LD} ${APP_IMAGE}
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
            --stage System/Boot.xoex=${EXEC_XOEX}
            --stage System/Programs/${DISK_APP_NAME}.xcex=${DISK_APP_XCEX}
            --stage System/Programs/svcd.xcex=${SVCD_XCEX}
            --stage System/Services/hello.xosv=${SRC_DIR}/os/services/hello.xosv
            --stage System/Services/disabled.xosv=${SRC_DIR}/os/services/disabled.xosv
    DEPENDS ${BOOT_BIN} ${STAGE2_BIN} ${KERNEL_XKEX} ${EXEC_XOEX} ${DISK_APP_XCEX}
            ${SVCD_XCEX} ${SRC_DIR}/os/services/hello.xosv
            ${SRC_DIR}/os/services/disabled.xosv ${CXK}
    COMMENT "Building unified CXK disk (XBPT: boot + stage + system)"
)
add_custom_target(cxk_disk ALL DEPENDS ${CXK_DISK})

# ---- image: boot + stage2 + kernel ----
add_custom_command(
    OUTPUT ${DISK_IMG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${IMAGES_DIR}
    COMMAND ${CMAKE_COMMAND} -E echo "Building v5 boot image..."
    COMMAND ${CXK} raw-image
        --boot ${BOOT_BIN}
        --stage2 ${STAGE2_BIN}
        --kernel ${KERNEL_XKEX}
        --out ${DISK_IMG}
    DEPENDS ${BOOT_BIN} ${STAGE2_BIN} ${KERNEL_XKEX} ${CXK}
    COMMENT "Imaging boot + stage 2 + kernel"
)
add_custom_target(disk_image ALL DEPENDS ${DISK_IMG})

# cxk_disk and disk_image are both ALL targets and both consume the same
# intermediates - every kernel object, the boot binaries, kernel.xkex. With the
# Makefile generator CMake copies the rule for a shared custom-command output
# into EVERY target that consumes it, so under -j the two targets run the same
# compile concurrently and one links an object the other is still writing.
#
# That presents as a link failure against a symbol that is plainly in the source
# - or, worse, as a kernel built from a half-written object that links fine.
# A serial build always worked, which is what made it look like a code bug.
#
# Ordering the two targets means the second finds every intermediate already up
# to date and skips it, so nothing is ever written twice at once. The file-level
# DEPENDS above are all kept, so a changed source still forces a rebuild.
add_dependencies(disk_image cxk_disk)

# ---- convenience: build just the X app chain ----
add_custom_target(apps DEPENDS ${APP_IMAGE})