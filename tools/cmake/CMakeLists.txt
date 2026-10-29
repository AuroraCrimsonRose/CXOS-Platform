# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
# ============================================================================
#  CXK - CMakeLists.txt
#  Copyright (c) 2026 CATX Systems.  All rights reserved.
#  Author: Aurora Tejeda / CATX Systems
# ============================================================================
#
# The x86-64 system - boot chain + kernel + X programs on one disk.
#   stage 1 (boot.asm) -> stage 2 (a20/mem/gdt/pmode, then long.asm: the ELF64
#   kernel placed and long mode entered) -> kernel (top 2 GiB) ->
#   executive (.xoex) -> shell (.xsex)
#
# Kernel sources are LISTS (KERNEL_C_SRCS / KERNEL_ASM_SRCS): to add a
# subsystem, append its .c/.asm here - the compile+link rules are generated
# automatically. Include dirs are in KERNEL_INCLUDES. Programs are one
# cx_x_program line each (cxk_toolchain.cmake).
#
# X programs build automatically: edit os/apps/shell.xfxn and a plain build
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
# is no cross GCC to install and no per-host prefix or .exe suffix to guess:
# the defaults below are correct on Windows, Linux and macOS alike.
#
# Each is still overridable with -D, for a host carrying several LLVM versions
# or one where they are not on PATH.
if(NOT DEFINED CXK_TARGET)
    set(CXK_TARGET "x86_64-elf")
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
set(IMAGES_DIR   ${SRC_DIR}/dist/CXK_x86_64/images)
set(PACKAGES_DIR ${SRC_DIR}/dist/CXK_x86_64/packages)

# ---- kernel include directories (add new subsystem dirs here) ----
set(KERNEL_INCLUDES
    -I${SRC_DIR}/abi
    -I${SRC_DIR}/kernel
    -I${SRC_DIR}/kernel/lib/time
    -I${SRC_DIR}/kernel/lib/string
    -I${SRC_DIR}/kernel/lib/math
    -I${SRC_DIR}/kernel/lib/crypto
    -I${SRC_DIR}/kernel/lib/mem
    -I${SRC_DIR}/kernel/lib/gfx
    -I${SRC_DIR}/kernel/lib/format
    -I${SRC_DIR}/kernel/drivers/storage
    -I${SRC_DIR}/kernel/drivers/storage/partition
    -I${SRC_DIR}/kernel/cpu
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

# ---- the kernel's own version, from the one registry ----
# Read straight out of versions.json rather than copied here, so the kernel
# cannot print a version the registry disagrees with - the banner used to be the
# string literal "CXK v5", which is how it stayed at v5 through every change.
# The fallback exists for a bare host-compile with no CMake, and is itself
# checked against the registry by `cxk check-versions`.
set(CXK_KERNEL_VERSION_FALLBACK "1.0.1")
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

# freestanding kernel C flags
#
# The kernel code model, and no red zone: interrupts land on the current stack
# in ring 0, below RSP.
#
# -mno-sse/-sse2/-mmx/-implicit-float are NOT optional and must never be dropped.
# Left alone, clang uses XMM registers for ordinary integer work - on x86-32 it
# emitted `movsd 0x40(%esp), %xmm0` in pmm_init purely to move a 64-bit value.
# The kernel saves no SIMD state on a context switch, so the first such
# instruction would corrupt another thread's registers, or raise #UD where the
# CPU has SSE switched off.
#
# -Wshorten-64-to-32 is an error: a pointer, a paddr_t or a size_t squeezed
# through uint32_t truncates silently. It was a separate syntax-only pass over
# the generic code while the 32-bit kernel was the build (roadmap Phase 0
# step 5); it is simply the kernel's own flags now.
#
# x87 stays ENABLED (no -mno-80387) for kernel/lib/math/kmath.c and arch/x86/fpu.c.
# Neither is in the build today, deliberately: fpu.c explains that enabling FP
# needs FPU state saved on context switch first. Leaving x87 on means wiring
# them in is a source-list change, not a flags change.
set(KCFLAGS --target=${CXK_TARGET} -ffreestanding -fno-pic -fno-stack-protector
            -mcmodel=kernel -mno-red-zone
            -mno-sse -mno-sse2 -mno-mmx -mno-implicit-float
            -Wall -Wextra -Werror -Wshorten-64-to-32 ${CXK_DEFS}
            ${KERNEL_INCLUDES} -I${BUILD_DIR} -c)

# ---- KERNEL C SOURCES (append new subsystems here) ----
set(KERNEL_C_SRCS
    ${SRC_DIR}/kernel/kmain.c
    ${SRC_DIR}/kernel/boot/cxbi.c
    ${SRC_DIR}/kernel/diag/crashrec.c
    ${SRC_DIR}/kernel/diag/crashlog.c
    ${SRC_DIR}/kernel/ktest.c
    ${SRC_DIR}/kernel/kconfig.c
    ${SRC_DIR}/kernel/arch/x86_64/cpu.c
    ${SRC_DIR}/kernel/arch/x86_64/early_con.c
    ${SRC_DIR}/kernel/arch/x86_64/ktest_arch.c
    ${SRC_DIR}/kernel/arch/x86_64/gdt.c
    ${SRC_DIR}/kernel/arch/x86_64/idt.c
    ${SRC_DIR}/kernel/arch/x86/apic.c
    ${SRC_DIR}/kernel/arch/x86/cpufeat.c
    ${SRC_DIR}/kernel/arch/x86/ktest_x86.c
    ${SRC_DIR}/kernel/arch/x86_64/paging.c
    ${SRC_DIR}/kernel/arch/x86_64/pagetable.c
    ${SRC_DIR}/kernel/memman/buddy.c
    ${SRC_DIR}/kernel/memman/pagedb.c
    ${SRC_DIR}/kernel/memman/heap.c
    ${SRC_DIR}/kernel/memman/vmregion.c
    ${SRC_DIR}/kernel/memman/kstack.c
    ${SRC_DIR}/kernel/memman/dma.c
    ${SRC_DIR}/kernel/lib/time/datetime.c
    ${SRC_DIR}/kernel/lib/string/string.c
    ${SRC_DIR}/kernel/lib/mem/bitmap.c
    ${SRC_DIR}/kernel/lib/string/format.c
    ${SRC_DIR}/kernel/lib/string/logging.c
    ${SRC_DIR}/kernel/lib/string/syslog.c
    ${SRC_DIR}/kernel/cpu/sched.c
    ${SRC_DIR}/kernel/cpu/uid.c
    ${SRC_DIR}/kernel/drivers/char/timer.c
    ${SRC_DIR}/kernel/cpu/usermode.c
    ${SRC_DIR}/kernel/cpu/usercopy.c
    ${SRC_DIR}/kernel/cpu/sysrec.c
    ${SRC_DIR}/kernel/cpu/systable.c
    ${SRC_DIR}/kernel/cpu/proc.c
    ${SRC_DIR}/kernel/cpu/ipc.c
    ${SRC_DIR}/kernel/cpu/spawn.c
    ${SRC_DIR}/kernel/cpu/syspage.c
    ${SRC_DIR}/kernel/cpu/sysfile.c
    ${SRC_DIR}/kernel/object/kobj.c
    ${SRC_DIR}/kernel/object/kwait.c
    ${SRC_DIR}/kernel/object/htab.c
    ${SRC_DIR}/kernel/memman/ocache.c
    ${SRC_DIR}/kernel/object/idtab.c
    ${SRC_DIR}/kernel/cpu/exec.c
    ${SRC_DIR}/kernel/cpu/keyvault.c
    ${SRC_DIR}/kernel/cpu/launch.c
    ${SRC_DIR}/kernel/drivers/char/console.c
    ${SRC_DIR}/kernel/drivers/char/serial.c
    ${SRC_DIR}/kernel/drivers/video/fb.c
    ${SRC_DIR}/kernel/drivers/input/keyboard.c
    ${SRC_DIR}/kernel/drivers/input/mouse.c
    ${SRC_DIR}/kernel/drivers/power/power.c
    ${SRC_DIR}/kernel/drivers/power/acpi.c
    ${SRC_DIR}/kernel/lib/gfx/color.c
    ${SRC_DIR}/kernel/lib/gfx/resolution.c
    ${SRC_DIR}/kernel/lib/gfx/font.c
    ${SRC_DIR}/kernel/lib/gfx/xfnt.c
    ${SRC_DIR}/kernel/lib/gfx/klogo.c
    ${SRC_DIR}/kernel/drivers/iowin.c
    ${SRC_DIR}/kernel/platform/pc/pic.c
    ${SRC_DIR}/kernel/platform/pc/pit.c
    ${SRC_DIR}/kernel/platform/pc/i8042.c
    ${SRC_DIR}/kernel/platform/pc/ps2kbd.c
    ${SRC_DIR}/kernel/platform/pc/ps2mouse.c
    ${SRC_DIR}/kernel/platform/pc/rtc.c
    ${SRC_DIR}/kernel/platform/pc/speaker.c
    ${SRC_DIR}/kernel/platform/pc/vga.c
    ${SRC_DIR}/kernel/platform/pc/emupower.c
    ${SRC_DIR}/kernel/platform/pc/platform.c
    ${SRC_DIR}/kernel/platform/pc/ktest_pc.c
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
    ${SRC_DIR}/kernel/drivers/usb/xhci_spad.c
    ${SRC_DIR}/kernel/drivers/usb/ohci.c
    ${SRC_DIR}/kernel/drivers/usb/usb_storage.c
    ${SRC_DIR}/kernel/drivers/usb/usb_hid.c
    ${SRC_DIR}/kernel/ktest_loader.c
    ${SRC_DIR}/kernel/ktest_net.c
    ${SRC_DIR}/kernel/ktest_fs.c
)

# ---- KERNEL ASM SOURCES (elf64 objects; append new ones here) ----
set(KERNEL_ASM_SRCS
    ${SRC_DIR}/kernel/arch/x86_64/entry.asm
    ${SRC_DIR}/kernel/arch/x86_64/isr.asm
    ${SRC_DIR}/kernel/arch/x86_64/switch.asm
    ${SRC_DIR}/kernel/arch/x86_64/usermode.asm
)

# ---- linker script ----
set(LINKER ${SRC_DIR}/kernel/arch/x86_64/linker.ld)

# ---- boot sources ----
set(BOOT_SRC   ${SRC_DIR}/boot/boot.asm)
set(STAGE2_SRC ${SRC_DIR}/boot/stage2.asm)

# ---- outputs ----
set(BOOT_BIN    ${BUILD_DIR}/boot.bin)
set(STAGE2_BIN  ${BUILD_DIR}/stage2.bin)
set(KERNEL_ELF  ${BUILD_DIR}/kernel.elf)
set(KERNEL_DISK_ELF ${BUILD_DIR}/kernel.stripped.elf)   # what the disk carries
set(CXK_DISK    ${IMAGES_DIR}/cxk_x86_64.img)

# Everything a program can pull in with `import`, so editing any of it actually
# rebuilds. This variable was once referenced by the compile rule but never
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
# The X compiler's own sources, which tokdump, astdump, semadump and xc import.
file(GLOB XC_SRCS CONFIGURE_DEPENDS ${SRC_DIR}/os/xc/*.xfxn ${SRC_DIR}/os/xc/cxk/*.xfxn)

include(${CMAKE_SOURCE_DIR}/cxk_toolchain.cmake)

# ---- generate compile rules for each kernel C source ----
#
# DEPFILE is load-bearing, not a nicety. These are custom commands, so the only
# dependency CMake knows about is the one named in DEPENDS - the .c file. Without
# a depfile, **editing a header rebuilt nothing at all**: the build reported
# success, ninja said "0/2 re-checking globbed directories", and the image kept
# whatever the headers used to say.
#
# That is worse than a change not taking effect. Headers here carry struct
# layouts, ABI constants and inline functions, so a stale object does not merely
# miss an edit - it disagrees with the objects that did get rebuilt, about the
# shape of memory they share. It also quietly defeats the verification standard
# this repository runs on: a sabotage in a header, rebuilt and booted, would come
# back green while the code under test was never compiled.
#
# Caught 2026-10-10 by the mtime habit the handoff already prescribes - the
# sabotage was in font.h, and build/kernel.elf came back older than the edit.
#
# clang writes the depfile with -MD -MF; CMake needs DEPFILE to read it back.
set(KERNEL_OBJS "")
foreach(src ${KERNEL_C_SRCS})
    get_filename_component(name ${src} NAME_WE)
    set(obj ${BUILD_DIR}/k_${name}.o)
    set(dep ${BUILD_DIR}/k_${name}.o.d)
    add_custom_command(
        OUTPUT ${obj}
        COMMAND ${KCC} ${KCFLAGS} -MD -MF ${dep} ${src} -o ${obj}
        DEPENDS ${src}
        DEPFILE ${dep}
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
        COMMAND ${CMAKE_ASM_NASM_COMPILER} -f elf64 ${src} -o ${obj}
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
# Its code is position-independent by construction - every reference X64Emitter
# writes is RIP-relative - so it links in the kernel's top 2 GiB as it does in a
# user program; it uses no SSE and lowers RSP before touching a local, so no
# red zone.
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
        COMMAND ${CXK} compile ${src} ${obj} --no-prelude --object --target x86_64
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
        ${SRC_DIR}/boot/a20.asm ${SRC_DIR}/boot/mem.asm ${SRC_DIR}/boot/cxbi.asm
        ${SRC_DIR}/boot/gdt.asm ${SRC_DIR}/boot/pmode.asm
        ${SRC_DIR}/boot/long.asm ${SRC_DIR}/boot/vbe.asm
    COMMENT "Assembling stage 2 (stage2.asm + includes)"
)

# ---- the architecture boundary ----
# No file outside kernel/arch/ may contain code for one CPU: inline assembly,
# an assembly source, a CPU intrinsic, or an include of arch/<name>/
# (check_arch_boundary.cmake says exactly what it refuses). The kernel does not
# link until it passes. It scans every file under kernel/, in the build or not,
# so new and unbuilt files are held to it too. (Globbed through ARCH_CHECK_KDIR
# because cxk's source pre-flight reads every path under SRC_DIR ending in .c
# here as a file the build lists, and a pattern is not one.)
set(ARCH_CHECK_KDIR ${SRC_DIR}/kernel)
file(GLOB_RECURSE ARCH_CHECK_FILES CONFIGURE_DEPENDS
     ${ARCH_CHECK_KDIR}/*.c ${ARCH_CHECK_KDIR}/*.h
     ${ARCH_CHECK_KDIR}/*.asm ${ARCH_CHECK_KDIR}/*.S ${ARCH_CHECK_KDIR}/*.s)
set(ARCH_CHECK_STAMP ${BUILD_DIR}/arch_boundary.ok)
add_custom_command(
    OUTPUT ${ARCH_CHECK_STAMP}
    COMMAND ${CMAKE_COMMAND} -DKDIR=${ARCH_CHECK_KDIR} -DSTAMP=${ARCH_CHECK_STAMP}
            -P ${CMAKE_CURRENT_LIST_DIR}/check_arch_boundary.cmake
    DEPENDS ${ARCH_CHECK_FILES} ${CMAKE_CURRENT_LIST_DIR}/check_arch_boundary.cmake
    COMMENT "Checking the architecture boundary"
)

# ---- link the kernel (top 2 GiB) ----
add_custom_command(
    OUTPUT ${KERNEL_ELF}
    COMMAND ${CMAKE_LD} -m elf_x86_64 -nostdlib -T ${LINKER} -o ${KERNEL_ELF} ${KERNEL_OBJS}
    DEPENDS ${KERNEL_OBJS} ${LINKER} ${ARCH_CHECK_STAMP}
    COMMENT "Linking the kernel (top 2 GiB)"
)
# The disk carries the same link with no symbol table: stage 2 reads the whole
# file into 512 KB of low memory (KERNEL_SECTORS in boot/stage2.asm) and
# loads only the PT_LOAD segments, so the symbols cost budget and buy nothing
# there. kernel.elf keeps them, for a debugger or addr2line.
#
# The kernel is an ELF image as it stands, unsigned: CXEX for x86-64, and the
# signature check in the loader, come with it (docs/boot/CX_BOOT.md section 4).
add_custom_command(
    OUTPUT ${KERNEL_DISK_ELF}
    COMMAND ${CMAKE_LD} -m elf_x86_64 -nostdlib --strip-all -T ${LINKER} -o ${KERNEL_DISK_ELF} ${KERNEL_OBJS}
    DEPENDS ${KERNEL_OBJS} ${LINKER} ${KERNEL_ELF}
    COMMENT "Linking the kernel for the disk (no symbols)"
)

# ---- console font: Terminus (OFL-1.1) BDF -> .xfnt, staged to /System/Fonts ----
#
# CXOS uses published fonts rather than drawing its own (2026-10-16). The BDF
# is Terminus 4.49.1 exactly as its author ships it; it is converted here, not
# checked in built, so the .xfnt can never drift from its source. The console's
# upper 128 bytes are IBM code page 437, and the conversion is strict: a font
# missing any character of the set fails the build with the codepoints named.
#
# Named console_16, face "Console", never "Terminus": Terminus reserves the
# name "Terminus Font", and under OFL-1.1 a converted or subset font is a
# Modified Version, which may not carry a Reserved Font Name.
set(FONT_DIR       ${SRC_DIR}/vendor/terminus)
set(CONSOLE16_BDF ${FONT_DIR}/ter-u16n.bdf)
set(CONSOLE16_XFNT ${BUILD_DIR}/console_16.xfnt)
add_custom_command(
    OUTPUT ${CONSOLE16_XFNT}
    COMMAND ${CXK} font build ${CONSOLE16_BDF} --charset cp437 --face Console -o ${CONSOLE16_XFNT}
    DEPENDS ${CONSOLE16_BDF} ${CXK}
    COMMENT "FONT ter-u16n.bdf -> console_16.xfnt (CP437)"
)

# ---- the X programs ----
# APP_BOOT is the startup app: embedded into the executive and spawned at boot.
# To swap shells, point APP_BOOT/APP_BOOT_NAME at a different .xfxn.
#
# Every other program is the opposite: staged onto the disk as a file and
# started with SYS_EXEC_PATH. Each has to be SIGNED, because the kernel
# verifies what it loads from disk - an unsigned program sitting on disk is
# refused, which is the property worth having. Above all for the supervisor,
# the process that decides what else gets to run.
#
# setcheck is the settings protocol's self-test. X code, so ktest cannot reach
# it - it runs as an ordinary ring-3 program at boot and prints a verdict line
# the serial log captures, which is the same contract a ktest has.
#
# tokdump, astdump and semadump are the X compiler's front end (os/xc), built
# with the CXK platform file and staged with the compiler's own sources, so
# `run tokdump -s /Shared/Source/lex.xfxn` on CXK can be checked against the
# same command on the host: the same counts and hash mean the same tokens.
# `run +disk semadump -s --prelude /Shared/Source/prelude.xfxn
# /Shared/Source/sema.xfxn` type checks the type checker on CXK. xc is the
# whole compiler, X source to assembly: in /Shared/Source,
# `run +disk xc --prelude prelude.xfxn xc.xfxn` compiles the compiler on CXK.
#
# They are linked at user addresses below 4 GB, which the ABI's u32 user
# addresses need ("Not in 6d", docs/planning/CX_ROADMAP.md).
set(APP_BOOT      ${SRC_DIR}/os/apps/shell.xfxn)
set(APP_BOOT_NAME shell)
set(DISK_APP_NAME hi)
set(XC_INCS ${SRC_DIR}/os/xc/cxk ${SRC_DIR}/os/xc ${SRC_DIR}/os/std)
cx_x_program(SHELL_XSEX      ${APP_BOOT}                          system xsex ${SRC_DIR}/os/std)
cx_x_program(DISK_APP_XUEX   ${SRC_DIR}/os/apps/hi.xfxn           user   xuex ${SRC_DIR}/os/std)
cx_x_program(SETCHECK_XUEX   ${SRC_DIR}/os/apps/setcheck.xfxn     user   xuex ${SRC_DIR}/os/std)
cx_x_program(SUPERVISOR_XSEX ${SRC_DIR}/os/apps/supervisor.xfxn   system xsex ${SRC_DIR}/os/std)
cx_x_program(TOKDUMP_XUEX    ${SRC_DIR}/os/xc/tokdump.xfxn        user   xuex ${XC_INCS})
cx_x_program(ASTDUMP_XUEX    ${SRC_DIR}/os/xc/astdump.xfxn        user   xuex ${XC_INCS})
cx_x_program(SEMADUMP_XUEX   ${SRC_DIR}/os/xc/semadump.xfxn       user   xuex ${XC_INCS})
cx_x_program(XC_XUEX         ${SRC_DIR}/os/xc/xc.xfxn             user   xuex ${XC_INCS})

# ---- shell.xsex -> app_image.h (byte array the executive embeds) ----
# The boot shell is EMBEDDED in the executive rather than staged on disk, and
# used to be the one artifact nobody signed - trusted only because the
# executive around it was. SYS_SPAWN now verifies what it is handed, so the
# shell has a signature of its own, made before `cxk embed` runs: the byte
# array the executive carries is the signed image.
set(APP_IMAGE ${BUILD_DIR}/app_image.h)   # executive.c includes <app_image.h>: -I the target's directory
add_custom_command(
    OUTPUT ${APP_IMAGE}
    COMMAND ${CXK} embed ${SHELL_XSEX} ${APP_IMAGE} ${APP_BOOT_NAME}_xsex
    DEPENDS ${SHELL_XSEX} ${CXK}
    COMMENT "Embedding ${APP_BOOT_NAME}.xsex -> app_image.h"
)

# ---- executive.c -> executive.elf (user half) -> executive.xoex (signed) ----
# No red zone: the executive's syscalls are inline `call`s, which push below
# RSP (executive.c).
set(EXEC_C    ${SRC_DIR}/os/executive/executive.c)
set(EXEC_LD   ${SRC_DIR}/os/executive/executive.ld)
set(EXEC_ELF  ${BUILD_DIR}/executive.elf)
set(EXEC_XOEX ${PACKAGES_DIR}/executive.xoex)
add_custom_command(
    OUTPUT ${EXEC_ELF}
    COMMAND ${KCC} --target=${CXK_TARGET} -ffreestanding -fno-pic -fno-stack-protector -nostdlib
            -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-implicit-float
            -I${SRC_DIR}/abi
            -I${BUILD_DIR}
            -fuse-ld=lld -Wl,-T,${EXEC_LD} -o ${EXEC_ELF} ${EXEC_C}
    DEPENDS ${EXEC_C} ${EXEC_LD} ${APP_IMAGE}
    COMMENT "CC+LD executive.c -> executive.elf (user-half .xoex bootstrap)"
)
cx_sign(EXEC_SIGN ${EXEC_XOEX})
add_custom_command(
    OUTPUT ${EXEC_XOEX}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
    COMMAND ${CXK} build ${EXEC_ELF} ${EXEC_XOEX} --type os
    ${EXEC_SIGN}
    DEPENDS ${EXEC_ELF} ${CXK}
    COMMENT "Converting executive.elf -> executive.xoex (CXEX, type os)"
)

# xc reads the prelude for the target it compiles for: what `cxk compile`
# puts in front of a program.
set(PRELUDE_XFXN ${BUILD_DIR}/prelude.xfxn)
add_custom_command(
    OUTPUT ${PRELUDE_XFXN}
    COMMAND ${CXK} prelude ${PRELUDE_XFXN} --target x86_64
    DEPENDS ${CXK}
    COMMENT "X   prelude -> prelude.xfxn"
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
    ${SRC_DIR}/os/services/setcheck.xosv
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
                COMMAND ${CXK} check-xdata --quiet --keys kernel_stack_kib,max_threads,max_handles,memory_quota_mib ${KERNEL_XKCO})
else()
    message(WARNING "this cxk predates check-xdata - service descriptors will not be checked "
                    "at build time. Republish cxk from the DevKit to enable it.")
    set(XDCHECK COMMAND ${CMAKE_COMMAND} -E echo "  service descriptors and kernel.xkco NOT checked - cxk predates check-xdata")
endif()

# What the system partition holds.
set(CXK_STAGED_PROGRAMS
    --stage System/Boot.xoex=${EXEC_XOEX}
    --stage Shared/Programs/${DISK_APP_NAME}.xuex=${DISK_APP_XUEX}
    --stage System/Programs/supervisor.xsex=${SUPERVISOR_XSEX}
    --stage Shared/Programs/tokdump.xuex=${TOKDUMP_XUEX}
    --stage Shared/Programs/astdump.xuex=${ASTDUMP_XUEX}
    --stage Shared/Programs/semadump.xuex=${SEMADUMP_XUEX}
    --stage Shared/Programs/xc.xuex=${XC_XUEX}
    --stage Shared/Programs/setcheck.xuex=${SETCHECK_XUEX}
    --stage Shared/Source/prelude.xfxn=${PRELUDE_XFXN}
)
set(CXK_STAGED_SHARED
    --stage System/Fonts/console_16.xfnt=${CONSOLE16_XFNT}
    --stage Shared/Source/xc.xfxn=${SRC_DIR}/os/xc/xc.xfxn
    --stage Shared/Source/front.xfxn=${SRC_DIR}/os/xc/front.xfxn
    --stage Shared/Source/ir.xfxn=${SRC_DIR}/os/xc/ir.xfxn
    --stage Shared/Source/x64.xfxn=${SRC_DIR}/os/xc/x64.xfxn
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
    --stage System/Services/hello.xosv=${SRC_DIR}/os/services/hello.xosv
    --stage System/Services/setcheck.xosv=${SRC_DIR}/os/services/setcheck.xosv
    --stage System/Services/disabled.xosv=${SRC_DIR}/os/services/disabled.xosv
    --stage System/Services/heartbeat.xosv=${SRC_DIR}/os/services/heartbeat.xosv
    --stage System/Config/kernel.xkco=${KERNEL_XKCO}
)

# ---- the disk: stage1 + XBPT + stage2 + BOOT/STAGE/SYSTEM partitions ----
add_custom_command(
    OUTPUT ${CXK_DISK}
    ${XDCHECK}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${IMAGES_DIR}
    COMMAND ${CXK} image --out ${CXK_DISK} --size-mb 64 --boot-mb 8
            --stage1 ${BOOT_BIN} --stage2 ${STAGE2_BIN} --kernel ${KERNEL_DISK_ELF}
            ${CXK_STAGED_PROGRAMS} ${CXK_STAGED_SHARED}
    DEPENDS ${BOOT_BIN} ${STAGE2_BIN} ${KERNEL_DISK_ELF} ${EXEC_XOEX} ${DISK_APP_XUEX}
            ${SUPERVISOR_XSEX} ${TOKDUMP_XUEX} ${ASTDUMP_XUEX} ${SEMADUMP_XUEX} ${XC_XUEX}
            ${SETCHECK_XUEX} ${PRELUDE_XFXN} ${XC_SRCS} ${STD_SRCS}
            ${SERVICE_XOSV} ${KERNEL_XKCO} ${CONSOLE16_XFNT} ${CXK}
    COMMENT "Building the CXK disk (XBPT: boot + stage + system)"
)
add_custom_target(cxk_disk ALL DEPENDS ${CXK_DISK})

# ---- convenience: build just the X app chain ----
add_custom_target(apps DEPENDS ${APP_IMAGE})
