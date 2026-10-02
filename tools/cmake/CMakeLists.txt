# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
# ============================================================================
#  CXK - CMakeLists.txt
#  Copyright (c) 2026 CATX Systems.  All rights reserved.
#  Author: Aurora Tejeda / CATX Systems
# ============================================================================
#
# v5 build - boot chain + higher-half kernel + X apps.
#   stage 1 (boot.asm) -> stage 2 (a20/mem/gdt/pmode + CXEX load) ->
#   kernel (higher-half at 0xC0100000) -> executive (.xoex) -> shell (.xsex)
#
# Kernel sources are LISTS (KERNEL_C_SRCS / KERNEL_ASM_SRCS): to add a
# subsystem, append its .c/.asm here - the compile+link rules are generated
# automatically. Include dirs are in KERNEL_INCLUDES.
#
# X apps build automatically: edit os/apps/shell.xfxn and a plain build
# recompiles it, repackages the .xsex, regenerates app_image.h, relinks the
# executive, and reimages the disk.

cmake_minimum_required(VERSION 3.19)   # string(JSON ...) reads versions.json
project(CXK ASM_NASM)
enable_language(ASM_NASM)

# ---- build-time flags (edit cmake/cxk_flags.cmake, NOT this file) ----
# One source of truth for toggles like CXK_ENABLE_FB. Each NAME=VALUE becomes
# -DNAME=VALUE and is fed to BOTH clang (kernel C) and nasm (boot + kernel asm),
# so the kernel and bootloader can't disagree.
include(${CMAKE_SOURCE_DIR}/cxk_flags.cmake)
set(CXK_DEFS "")
foreach(f ${CXK_FLAGS})
    list(APPEND CXK_DEFS -D${f})
endforeach()

# ---- development kernels ----
# -DDEV_UNSIGNED=ON builds a kernel that runs images carrying NO signature at
# all, so testing a change does not need a signing key generated, embedded and
# destroyed every time. It is a BUILD option rather than a runtime or boot
# setting on purpose: a switch a running system could flip is exactly what an
# attacker wants, while a release kernel built without this simply contains no
# code that can run an unsigned image.
#
# It lives here on the command line, not in cxk_flags.cmake, so it can never be
# committed switched on. And it admits only UNSIGNED - a tampered image, a
# forged signature or a file that is not a program is refused as always.
option(DEV_UNSIGNED "development kernel: run unsigned images - never ship one" OFF)
if(DEV_UNSIGNED)
    list(APPEND CXK_DEFS -DCXK_DEV_UNSIGNED=1)
    message(WARNING "DEV_UNSIGNED=ON: this kernel runs UNSIGNED images. "
                    "It is for testing only and must never be shipped.")
endif()

# ---- toolchain (LLVM: clang + ld.lld, HARDENING_PLAN D5) ----
# One toolchain on every host. clang cross-compiles by target triple, so there
# is no i686-elf GCC to install and no per-host prefix or .exe suffix to guess:
# the defaults below are correct on Windows, Linux and macOS alike.
#
# Each is still overridable with -D, for a host carrying several LLVM versions
# or one where they are not on PATH.
if(NOT DEFINED CXK_TARGET)
    set(CXK_TARGET "i686-elf")
endif()
if(NOT DEFINED CMAKE_ASM_NASM_COMPILER)
    set(CMAKE_ASM_NASM_COMPILER nasm)
endif()
if(NOT DEFINED KCC)
    set(KCC clang)
endif()
if(NOT DEFINED CMAKE_LD)
    set(CMAKE_LD ld.lld)
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
#
# -mno-sse/-sse2/-mmx/-implicit-float are NOT optional and must never be dropped.
# clang --target=i686-elf defaults to an SSE2-capable CPU, where the i686-elf GCC
# this replaced defaulted to plain i686. Left alone, clang uses XMM registers for
# ordinary integer work - it emitted `movsd 0x40(%esp), %xmm0` in pmm_init purely
# to move a 64-bit value. The kernel never sets CR4.OSFXSR, so the first such
# instruction raises #UD, and the invalid-opcode panic then faults again inside
# its own stack walker. The result is an early triple-fault-ish panic loop with
# no obvious connection to the compiler.
#
# x87 stays ENABLED (no -mno-80387): kernel/lib/math/kmath.c genuinely uses
# double, and cpu/fpu.c initialises the FPU for exactly that.
set(KCFLAGS --target=${CXK_TARGET} -ffreestanding -fno-pic -fno-stack-protector
            -mno-sse -mno-sse2 -mno-mmx -mno-implicit-float
            -Wall -Wextra ${CXK_DEFS} ${KERNEL_INCLUDES} -c)

# ---- the kernel's own version, from the one registry ----
# Read straight out of versions.json rather than copied here, so the kernel
# cannot print a version the registry disagrees with - the banner used to be the
# string literal "CXK v5", which is how it stayed at v5 through every change.
# The fallback exists for a bare host-compile with no CMake, and is itself
# checked against the registry by `cxk check-versions`.
set(CXK_KERNEL_VERSION_FALLBACK "1.0.0")
set(CXK_KERNEL_VERSION "${CXK_KERNEL_VERSION_FALLBACK}")
if(EXISTS ${SRC_DIR}/versions.json)
    file(READ ${SRC_DIR}/versions.json CXK_VERSIONS_JSON)
    string(JSON CXK_KERNEL_VERSION ERROR_VARIABLE CXK_JSON_ERR
           GET "${CXK_VERSIONS_JSON}" components cxk-kernel version)
    if(CXK_JSON_ERR OR NOT CXK_KERNEL_VERSION)
        message(FATAL_ERROR "versions.json: could not read components.cxk-kernel.version: ${CXK_JSON_ERR}")
    endif()
endif()
message(STATUS "CXK kernel version: ${CXK_KERNEL_VERSION}")
# A generated HEADER, not -DCXK_VERSION_STR=\"...\": the quotes a C string needs
# do not survive the trip into a custom-command argument list, so the define
# arrived unquoted and the banner failed to compile. A header has no quoting
# problem to get wrong. It goes in BUILD_DIR because it is generated output.
file(WRITE ${BUILD_DIR}/cxk_version.h
     "/* GENERATED from versions.json by tools/cmake/CMakeLists.txt - DO NOT EDIT */\n"
     "#ifndef CXK_VERSION_H\n#define CXK_VERSION_H\n"
     "#define CXK_VERSION_STR \"${CXK_KERNEL_VERSION}\"\n"
     "#endif\n")
list(APPEND KCFLAGS -I${BUILD_DIR})

# ---- KERNEL C SOURCES (append new subsystems here) ----
set(KERNEL_C_SRCS
    ${SRC_DIR}/kernel/kmain.c
    ${SRC_DIR}/kernel/ktest.c
    ${SRC_DIR}/kernel/kconfig.c
    ${SRC_DIR}/kernel/cpu/gdt.c
    ${SRC_DIR}/kernel/cpu/int/idt.c
    ${SRC_DIR}/kernel/cpu/int/pic.c
    ${SRC_DIR}/kernel/cpu/int/apic.c
    ${SRC_DIR}/kernel/memman/pmm.c
    ${SRC_DIR}/kernel/memman/paging.c
    ${SRC_DIR}/kernel/memman/addr_space.c
    ${SRC_DIR}/kernel/memman/heap.c
    ${SRC_DIR}/kernel/memman/vmregion.c
    ${SRC_DIR}/kernel/memman/kstack.c
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
    ${SRC_DIR}/kernel/cpu/keyvault.c
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
    ${BUILD_DIR}/trusted_key.c       # generated from tools/${CXK_KEY}.xkpk - cxk_toolchain.cmake
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
    ${SRC_DIR}/kernel/ktest_loader.c
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
set(LINKER ${SRC_DIR}/kernel/linker.ld)

# ---- boot sources ----
set(BOOT_SRC   ${SRC_DIR}/boot/boot.asm)
set(STAGE2_SRC ${SRC_DIR}/boot/stage2.asm)

# ---- outputs ----
set(BOOT_BIN    ${BUILD_DIR}/boot.bin)
set(STAGE2_BIN  ${BUILD_DIR}/stage2.bin)
set(KERNEL_ELF  ${BUILD_DIR}/kernel.elf)
set(KERNEL_XKEX ${DIST_DIR}/kernel.xkex)
set(DISK_IMG    ${IMAGES_DIR}/cxk_x86_32.img)

# ---- X apps: .xfxn -> .elf -> .xsex -> embedded C header ----
# APP_BOOT is the startup app: embedded into the executive and spawned at boot.
# To swap shells, point APP_BOOT/APP_BOOT_NAME at a different .xfxn.
set(APP_BOOT      ${SRC_DIR}/os/apps/shell.xfxn)
set(APP_BOOT_NAME shell)

set(APP_ELF   ${BUILD_DIR}/${APP_BOOT_NAME}.elf)
set(APP_XCEX  ${BUILD_DIR}/${APP_BOOT_NAME}.xsex)
set(APP_IMAGE ${SRC_DIR}/os/executive/app_image.h)

# DISK_APP is the opposite of APP_BOOT: not embedded in the executive, but
# staged onto the disk as a file and started with SYS_EXEC_PATH. It has to be
# SIGNED, because the kernel verifies what it loads from disk - an unsigned
# .xuex sitting on disk is refused, which is the property worth having.
set(DISK_APP      ${SRC_DIR}/os/apps/hi.xfxn)
set(DISK_APP_NAME hi)
set(DISK_APP_ELF  ${BUILD_DIR}/${DISK_APP_NAME}.elf)
set(DISK_APP_XCEX ${PACKAGES_DIR}/${DISK_APP_NAME}.xuex)

# supervisor, the service supervisor. Staged and signed exactly like DISK_APP - the
# executive starts it with SYS_EXEC_PATH, so the kernel verifies it on the way
# in and an unsigned one is refused. That is the property worth having for the
# process that decides what else gets to run.
set(SUPERVISOR_SRC  ${SRC_DIR}/os/apps/supervisor.xfxn)
set(SUPERVISOR_ELF  ${BUILD_DIR}/supervisor.elf)
set(SUPERVISOR_XCEX ${PACKAGES_DIR}/supervisor.xsex)

# tokdump, the token printer of the X compiler written in X (os/xc). Built with
# the CXK platform file, staged with a copy of the lexer's own source to read,
# so `run tokdump -s /Shared/Source/lex.xfxn` on CXK can be checked against the
# same command on the host: the same counts and hash mean the same tokens.
set(TOKDUMP_SRC  ${SRC_DIR}/os/xc/tokdump.xfxn)
set(TOKDUMP_ELF  ${BUILD_DIR}/tokdump.elf)
set(TOKDUMP_XUEX ${PACKAGES_DIR}/tokdump.xuex)
# astdump, the same for its parser: `run +disk astdump -s /Shared/Source/parse.xfxn`.
set(ASTDUMP_SRC  ${SRC_DIR}/os/xc/astdump.xfxn)
set(ASTDUMP_ELF  ${BUILD_DIR}/astdump.elf)
set(ASTDUMP_XUEX ${PACKAGES_DIR}/astdump.xuex)
# semadump, the same for its type checker: staged with the compiler's own
# sources and the prelude, so
# `run +disk semadump -s --prelude /Shared/Source/prelude.xfxn /Shared/Source/sema.xfxn`
# type checks the type checker on CXK.
set(SEMADUMP_SRC  ${SRC_DIR}/os/xc/semadump.xfxn)
set(SEMADUMP_ELF  ${BUILD_DIR}/semadump.elf)
set(SEMADUMP_XUEX ${PACKAGES_DIR}/semadump.xuex)
set(PRELUDE_XFXN  ${BUILD_DIR}/prelude.xfxn)   # what `cxk compile` puts in front of a program
# xc, the X compiler written in X: X source to assembly. Staged with all of
# its own source - the CXK platform file as xc_sys.xfxn - so that, in
# /Shared/Source, `run +disk xc --prelude prelude.xfxn xc.xfxn` compiles the
# compiler on CXK: the same assembly, byte for byte, as on the build machine.
set(XC_SRC  ${SRC_DIR}/os/xc/xc.xfxn)
set(XC_ELF  ${BUILD_DIR}/xc.elf)
set(XC_XUEX ${PACKAGES_DIR}/xc.xuex)

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

# ---- X linked into the kernel ----
# X compiled to plain objects with `cxk compile --object` and linked like any
# other kernel object; C calls it through a header (see kernel/lib/format/xdata.h).
# The X Data reader is the first: the kernel reads its configuration with the
# same code the supervisor uses, rather than a third implementation of it.
#
# Unlike the pre-flight checks this cannot be skipped on an older cxk - the
# kernel will not link without it - so an older cxk is stopped here, at
# configure time, with the fix, rather than at link time with a missing symbol.
execute_process(COMMAND ${CXK} compile --help
                OUTPUT_VARIABLE CXK_COMPILE_HELP ERROR_QUIET RESULT_VARIABLE CXK_COMPILE_RC)
if(NOT CXK_COMPILE_HELP MATCHES "--object")
    message(FATAL_ERROR "this cxk (${CXK}) cannot compile X to an object (no --object), "
                        "which the kernel now needs. Republish cxk from devkit/ (or take a newer release) - see README.md, Building.")
endif()
set(KERNEL_X_SRCS
    ${SRC_DIR}/os/std/xdata.xfxn
)
foreach(src ${KERNEL_X_SRCS})
    get_filename_component(name ${src} NAME_WE)
    set(obj ${BUILD_DIR}/kx_${name}.o)
    add_custom_command(
        OUTPUT ${obj}
        COMMAND ${CXK} compile ${src} ${obj} --no-prelude --object
        DEPENDS ${src} ${CXK}
        COMMENT "X   ${name}.xfxn (linked into the kernel)"
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
    COMMAND ${CMAKE_LD} -m elf_i386 -T ${LINKER} -o ${KERNEL_ELF} ${KERNEL_OBJS}
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

# ---- shell.elf -> shell.xsex (CXEX, type system) ----
add_custom_command(
    OUTPUT ${APP_XCEX}
    COMMAND ${CXK} build ${APP_ELF} ${APP_XCEX} --type system
    DEPENDS ${APP_ELF} ${CXK}
    COMMENT "Packaging ${APP_BOOT_NAME}.xsex (CXEX, type system)"
)

# ---- shell.xsex -> app_image.h (byte array the executive embeds) ----
add_custom_command(
    OUTPUT ${APP_IMAGE}
    COMMAND ${CXK} embed ${APP_XCEX} ${APP_IMAGE} ${APP_BOOT_NAME}_xsex
    DEPENDS ${APP_XCEX} ${CXK}
    COMMENT "Embedding ${APP_BOOT_NAME}.xsex -> app_image.h"
)

# ---- disk app: hi.xfxn -> ELF -> signed .xuex (staged, not embedded) ----
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
    COMMENT "Packaging ${DISK_APP_NAME}.xuex (CXEX, type user, staged to disk)"
)

# ---- supervisor: the service supervisor (staged + signed, like the disk app) ----
add_custom_command(
    OUTPUT ${SUPERVISOR_ELF}
    COMMAND ${CXK} compile ${SUPERVISOR_SRC} ${SUPERVISOR_ELF} -I ${SRC_DIR}/os/std
    DEPENDS ${SUPERVISOR_SRC} ${CXK} ${STD_SRCS}
    COMMENT "X   supervisor.xfxn -> ELF"
)
add_custom_command(
    OUTPUT ${SUPERVISOR_XCEX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
    COMMAND ${CXK} build ${SUPERVISOR_ELF} ${SUPERVISOR_XCEX} --type system
    ${VSIGN}
    DEPENDS ${SUPERVISOR_ELF} ${CXK}
    COMMENT "Packaging supervisor.xsex (CXEX, type system, staged to disk)"
)

# ---- tokdump, astdump, semadump: the X compiler's front end, on CXK (staged + signed, like the disk app) ----
file(GLOB XC_SRCS CONFIGURE_DEPENDS ${SRC_DIR}/os/xc/*.xfxn ${SRC_DIR}/os/xc/cxk/*.xfxn)
add_custom_command(
    OUTPUT ${TOKDUMP_ELF}
    COMMAND ${CXK} compile ${TOKDUMP_SRC} ${TOKDUMP_ELF}
            -I ${SRC_DIR}/os/xc/cxk -I ${SRC_DIR}/os/xc -I ${SRC_DIR}/os/std
    DEPENDS ${XC_SRCS} ${CXK} ${STD_SRCS}
    COMMENT "X   tokdump.xfxn -> ELF"
)
add_custom_command(
    OUTPUT ${TOKDUMP_XUEX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
    COMMAND ${CXK} build ${TOKDUMP_ELF} ${TOKDUMP_XUEX} --type user
    ${TSIGN}
    DEPENDS ${TOKDUMP_ELF} ${CXK}
    COMMENT "Packaging tokdump.xuex (CXEX, type user, staged to disk)"
)
add_custom_command(
    OUTPUT ${ASTDUMP_ELF}
    COMMAND ${CXK} compile ${ASTDUMP_SRC} ${ASTDUMP_ELF}
            -I ${SRC_DIR}/os/xc/cxk -I ${SRC_DIR}/os/xc -I ${SRC_DIR}/os/std
    DEPENDS ${XC_SRCS} ${CXK} ${STD_SRCS}
    COMMENT "X   astdump.xfxn -> ELF"
)
add_custom_command(
    OUTPUT ${ASTDUMP_XUEX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
    COMMAND ${CXK} build ${ASTDUMP_ELF} ${ASTDUMP_XUEX} --type user
    ${PSIGN}
    DEPENDS ${ASTDUMP_ELF} ${CXK}
    COMMENT "Packaging astdump.xuex (CXEX, type user, staged to disk)"
)
add_custom_command(
    OUTPUT ${XC_ELF}
    COMMAND ${CXK} compile ${XC_SRC} ${XC_ELF}
            -I ${SRC_DIR}/os/xc/cxk -I ${SRC_DIR}/os/xc -I ${SRC_DIR}/os/std
    DEPENDS ${XC_SRCS} ${CXK} ${STD_SRCS}
    COMMENT "X   xc.xfxn -> ELF"
)
add_custom_command(
    OUTPUT ${XC_XUEX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
    COMMAND ${CXK} build ${XC_ELF} ${XC_XUEX} --type user
    ${CSIGN}
    DEPENDS ${XC_ELF} ${CXK}
    COMMENT "Packaging xc.xuex (CXEX, type user, staged to disk)"
)
add_custom_command(
    OUTPUT ${PRELUDE_XFXN}
    COMMAND ${CXK} prelude ${PRELUDE_XFXN}
    DEPENDS ${CXK}
    COMMENT "X   prelude -> prelude.xfxn"
)
add_custom_command(
    OUTPUT ${SEMADUMP_ELF}
    COMMAND ${CXK} compile ${SEMADUMP_SRC} ${SEMADUMP_ELF}
            -I ${SRC_DIR}/os/xc/cxk -I ${SRC_DIR}/os/xc -I ${SRC_DIR}/os/std
    DEPENDS ${XC_SRCS} ${CXK} ${STD_SRCS}
    COMMENT "X   semadump.xfxn -> ELF"
)
add_custom_command(
    OUTPUT ${SEMADUMP_XUEX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
    COMMAND ${CXK} build ${SEMADUMP_ELF} ${SEMADUMP_XUEX} --type user
    ${MSIGN}
    DEPENDS ${SEMADUMP_ELF} ${CXK}
    COMMENT "Packaging semadump.xuex (CXEX, type user, staged to disk)"
)

# ---- executive.c -> executive.elf (user-half) -> executive.xoex (signed) ----
add_custom_command(
    OUTPUT ${EXEC_ELF}
    COMMAND ${KCC} --target=${CXK_TARGET} -ffreestanding -fno-pic -fno-stack-protector -nostdlib
            -mno-sse -mno-sse2 -mno-mmx -mno-implicit-float
            -I${SRC_DIR}/abi
            -fuse-ld=lld -Wl,-T,${EXEC_LD} -o ${EXEC_ELF} ${EXEC_C}
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

# ---- service descriptors: checked before they go on the disk ----
# The supervisor reads these at boot and reports a bad one as a line in its log.
# Checking them here turns that into a build error naming file:line:col. The
# DevKit's reader is a port of os/std/xdata.xfxn, held to the same results by
# its differential test, so what passes here the supervisor accepts. The key
# list is the supervisor's own - keep it in step with svc_keys_known().
#
# Probed rather than called outright, as `cxk os build` does for check-abi: a cxk
# published before check-xdata existed would fail on an unknown command and
# break a build that is otherwise fine. So it warns and skips until cxk is
# republished from the DevKit; a bad descriptor is a hard failure after that.
set(SERVICE_XOSV
    ${SRC_DIR}/os/services/hello.xosv
    ${SRC_DIR}/os/services/disabled.xosv
    ${SRC_DIR}/os/services/heartbeat.xosv)
# The kernel's configuration, checked the same way. Its keys are kconfig.c's:
# keep them in step with kconfig_parse().
set(KERNEL_XKCO ${SRC_DIR}/os/config/kernel.xkco)
execute_process(COMMAND ${CXK} check-xdata --help
                RESULT_VARIABLE CXK_HAS_XDATA OUTPUT_QUIET ERROR_QUIET)
if(CXK_HAS_XDATA EQUAL 0)
    set(XDCHECK COMMAND ${CXK} check-xdata --quiet --keys exec,args,start,every,grants ${SERVICE_XOSV}
                COMMAND ${CXK} check-xdata --quiet --keys kernel_stack_kib,max_threads,memory_quota_mib ${KERNEL_XKCO})
else()
    message(WARNING "this cxk predates check-xdata - service descriptors will not be checked "
                    "at build time. Republish cxk from the DevKit to enable it.")
    set(XDCHECK COMMAND ${CMAKE_COMMAND} -E echo "  service descriptors and kernel.xkco NOT checked - cxk predates check-xdata")
endif()

# ---- unified disk: stage1 + XBPT + stage2 + BOOT/STAGE/SYSTEM partitions ----
add_custom_command(
    OUTPUT ${CXK_DISK}
    ${XDCHECK}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${IMAGES_DIR}
    COMMAND ${CXK} image --out ${CXK_DISK} --size-mb 64 --boot-mb 8
            --stage1 ${BOOT_BIN} --stage2 ${STAGE2_BIN} --kernel ${KERNEL_XKEX}
            --stage System/Boot.xoex=${EXEC_XOEX}
            --stage Shared/Programs/${DISK_APP_NAME}.xuex=${DISK_APP_XCEX}
            --stage System/Programs/supervisor.xsex=${SUPERVISOR_XCEX}
            --stage Shared/Programs/tokdump.xuex=${TOKDUMP_XUEX}
            --stage Shared/Programs/astdump.xuex=${ASTDUMP_XUEX}
            --stage Shared/Programs/semadump.xuex=${SEMADUMP_XUEX}
            --stage Shared/Programs/xc.xuex=${XC_XUEX}
            --stage Shared/Source/xc.xfxn=${SRC_DIR}/os/xc/xc.xfxn
            --stage Shared/Source/front.xfxn=${SRC_DIR}/os/xc/front.xfxn
            --stage Shared/Source/emit.xfxn=${SRC_DIR}/os/xc/emit.xfxn
            --stage Shared/Source/dumputil.xfxn=${SRC_DIR}/os/xc/dumputil.xfxn
            --stage Shared/Source/xc_sys.xfxn=${SRC_DIR}/os/xc/cxk/xc_sys.xfxn
            --stage Shared/Source/io.xfxn=${SRC_DIR}/os/std/io.xfxn
            --stage Shared/Source/file.xfxn=${SRC_DIR}/os/std/file.xfxn
            --stage Shared/Source/string.xfxn=${SRC_DIR}/os/std/string.xfxn
            --stage Shared/Source/lex.xfxn=${SRC_DIR}/os/xc/lex.xfxn
            --stage Shared/Source/parse.xfxn=${SRC_DIR}/os/xc/parse.xfxn
            --stage Shared/Source/diag.xfxn=${SRC_DIR}/os/xc/diag.xfxn
            --stage Shared/Source/map.xfxn=${SRC_DIR}/os/xc/map.xfxn
            --stage Shared/Source/sema.xfxn=${SRC_DIR}/os/xc/sema.xfxn
            --stage Shared/Source/buf.xfxn=${SRC_DIR}/os/std/buf.xfxn
            --stage Shared/Source/heap.xfxn=${SRC_DIR}/os/std/heap.xfxn
            --stage Shared/Source/mem.xfxn=${SRC_DIR}/os/std/mem.xfxn
            --stage Shared/Source/prelude.xfxn=${PRELUDE_XFXN}
            --stage System/Services/hello.xosv=${SRC_DIR}/os/services/hello.xosv
            --stage System/Services/disabled.xosv=${SRC_DIR}/os/services/disabled.xosv
            --stage System/Services/heartbeat.xosv=${SRC_DIR}/os/services/heartbeat.xosv
            --stage System/Config/kernel.xkco=${KERNEL_XKCO}
    DEPENDS ${BOOT_BIN} ${STAGE2_BIN} ${KERNEL_XKEX} ${EXEC_XOEX} ${DISK_APP_XCEX}
            ${SUPERVISOR_XCEX} ${TOKDUMP_XUEX} ${ASTDUMP_XUEX} ${SEMADUMP_XUEX} ${XC_XUEX}
            ${XC_SRCS} ${SRC_DIR}/os/std/buf.xfxn ${SRC_DIR}/os/std/heap.xfxn
            ${SRC_DIR}/os/std/mem.xfxn ${PRELUDE_XFXN} ${STD_SRCS}
            ${SERVICE_XOSV} ${KERNEL_XKCO} ${CXK}
    COMMENT "Building unified CXK disk (XBPT: boot + stage + system)"
)
add_custom_target(cxk_disk ALL DEPENDS ${CXK_DISK})

# ---- image: boot + stage2 + kernel ----
add_custom_command(
    OUTPUT ${DISK_IMG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${IMAGES_DIR}
    COMMAND ${CMAKE_COMMAND} -E echo "Building boot image..."
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
