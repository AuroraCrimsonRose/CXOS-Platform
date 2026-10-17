# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
#
# Kernel code, compiled for the HOST and tested under AddressSanitizer and
# UndefinedBehaviorSanitizer (docs/safety/CX_SAFETY.md section 3.7).
#
# A separate CMake project from tools/cmake: that one cross-compiles the
# kernel for i686-elf, this one builds an ordinary host executable from the
# same sources. Only code with no hardware dependency belongs in KERNEL_SOURCES;
# anything that touches ports, page tables or interrupts is tested by booting.
#
#   cmake -S kernel/test/host -B build/hosttest -G Ninja -DCMAKE_C_COMPILER=clang
#   ninja -C build/hosttest && build/hosttest/khost_tests
#
# CXEX.Tests' KernelHostTests runs exactly that as part of `dotnet test`.
# On Windows the sanitizer runtime is a DLL in clang's resource directory
# (lib/windows); that test puts it on PATH for the run.

cmake_minimum_required(VERSION 3.20)
project(cxk_host_tests C)

set(KDIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)

set(KERNEL_SOURCES
    ${KDIR}/lib/mem/bitmap.c
    ${KDIR}/lib/crypto/sha256.c
    ${KDIR}/object/kobj.c
    ${KDIR}/object/htab.c
    ${KDIR}/object/idtab.c
    ${KDIR}/memman/ocache.c
    ${KDIR}/arch/x86/cpufeat.c
    ${KDIR}/arch/x86_32/pagetable.c
)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)

add_executable(khost_tests ${CMAKE_CURRENT_SOURCE_DIR}/runner.c ${TEST_SOURCES} ${KERNEL_SOURCES})

target_include_directories(khost_tests PRIVATE ${KDIR} ${KDIR}/../abi)
# A 2 MB window instead of the kernel's 64 MB, so a test can fill it.
target_compile_definitions(khost_tests PRIVATE CXK_HOST_TEST OCACHE_SLOTS=64)
set_property(TARGET khost_tests PROPERTY C_STANDARD 11)

target_compile_options(khost_tests PRIVATE
    -g -O1 -Wall -Wextra -Werror
    -fno-omit-frame-pointer
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
)
target_link_options(khost_tests PRIVATE -fsanitize=address,undefined)

if(WIN32)
    # clang targeting MSVC warns about the CRT's own declarations otherwise.
    target_compile_definitions(khost_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
