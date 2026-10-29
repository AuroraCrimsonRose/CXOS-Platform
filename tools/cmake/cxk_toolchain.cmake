# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
# ============================================================================
# cxk_toolchain.cmake  -  CXEX toolchain via the cxk CLI
# ============================================================================

# 1. Grab the tools folder path dynamically relative to THIS script
# This file is in:  tools/cmake
# One folder up:    tools
get_filename_component(CXK_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# 2. The cxk CLI. `cxk os build` passes -DCXK pointing at the running executable,
# which is the right answer and the usual case. This fallback is for a bare CMake
# invocation, and is platform-aware because the old one always ended in .exe -
# which made a direct `cmake tools/cmake` configure fail on Linux and macOS for a
# reason the error message did not explain.
if(NOT DEFINED CXK)
    if(CMAKE_HOST_WIN32)
        set(CXK "${CXK_TOOLS_DIR}/cxk.exe")
    else()
        set(CXK "${CXK_TOOLS_DIR}/cxk")
    endif()
endif()

# ---- code signing ----
option(SIGN "sign the CXEX programs with the key the kernel trusts" OFF)

# A signed kernel that runs unsigned code is the one artifact this must never
# produce: it looks official, and it trusts nothing. Refuse the combination at
# configure time rather than hope nobody builds it.
if(SIGN AND DEV_UNSIGNED)
    message(FATAL_ERROR "SIGN and DEV_UNSIGNED are mutually exclusive: together they would "
                        "produce a signed kernel that runs unsigned code. Pick one.")
endif()

# 3. The signing key pair, named by basename in the tools directory.
#
# One variable for both halves, because they must be halves of the SAME key:
# CXSigner refuses to sign with a private key that does not match the public key
# travelling with the image, and the kernel would refuse the result anyway. A
# second key is therefore selected by name - -DCXK_KEY=test uses
# tools/test.xksk + tools/test.xkpk - and never by pointing the two paths
# somewhere independently.
set(CXK_KEY "kernel" CACHE STRING "basename of the signing key pair in tools/ (kernel, test, ...)")
set(SIGN_SK   "${CXK_TOOLS_DIR}/${CXK_KEY}.xksk")
set(SIGN_PK   "${CXK_TOOLS_DIR}/${CXK_KEY}.xkpk")

# The kernel's compiled-in root of trust, GENERATED from the public half above.
#
# It used to be a tracked-but-gitignored file in the source tree that you ran
# `cxk embed` over once by hand. That made the one invariant that matters here
# impossible to enforce: the key the kernel trusts has to be the key the build
# signs with. Embed kernel.xkpk, sign with test.xksk, and every artifact is
# refused at boot as BAD_SIGNATURE - which reads as tampering, not as the wrong
# key on a command line, and sends you looking in the crypto instead of at the
# build. Generated from ${SIGN_PK} the two cannot disagree, and the manual step
# is gone with it.
if(NOT EXISTS ${SIGN_PK})
    message(FATAL_ERROR "no public key to trust: ${SIGN_PK}\n"
                        "  run: cxk keygen ${CXK_TOOLS_DIR}/${CXK_KEY}")
endif()
set(TRUSTED_KEY_C ${BUILD_DIR}/trusted_key.c)
add_custom_command(
    OUTPUT ${TRUSTED_KEY_C}
    COMMAND ${CXK} embed ${SIGN_PK} ${TRUSTED_KEY_C} cxos_trusted_key --extern
    DEPENDS ${SIGN_PK} ${CXK}
    COMMENT "Embedding ${CXK_KEY}.xkpk as the kernel's root of trust"
)

if(SIGN AND NOT EXISTS ${SIGN_SK})
    message(FATAL_ERROR "SIGN=ON but signing key missing: ${SIGN_SK}\n  run: tools/cxk.exe keygen tools/kernel")
endif()

# 4. The sign step for one artifact: a command naming the exact file. One per
# ARTIFACT, never per kind - a shared variable once signed hi.xuex twice and
# left the next program unsigned, which showed up as a signature refusal at
# boot rather than as a build error.
#
# No parentheses OR semicolons in the messages: they expand into a
# custom-command line, and under /bin/sh unquoted parens are a syntax error
# while a semicolon silently splits the command - the text after it just
# disappears. Both are harmless under cmd.exe, so the rule costs nothing on
# Windows. A development kernel runs these unsigned, so the release build's
# "WILL REFUSE" would be false there - and a false warning sends someone
# chasing a problem that does not exist.
function(cx_sign out file)
    get_filename_component(name ${file} NAME)
    if(SIGN)
        set(${out} COMMAND ${CXK} sign ${file} ${SIGN_SK} ${SIGN_PK} PARENT_SCOPE)
    elseif(DEV_UNSIGNED)
        set(${out} COMMAND ${CMAKE_COMMAND} -E echo "  ${name} UNSIGNED - runs on this development kernel only" PARENT_SCOPE)
    else()
        set(${out} COMMAND ${CMAKE_COMMAND} -E echo "  ${name} UNSIGNED - the kernel WILL REFUSE it - configure -DSIGN=ON" PARENT_SCOPE)
    endif()
endfunction()

# 5. An X program: <name>.xfxn -> ELF64 -> signed CXEX in PACKAGES_DIR, with
# `cxk compile --target x86_64` (the System V back end on the IR) and
# `cxk build` (an ELF64 becomes a CXEX with arch_target 2). Sets <var> to the
# package's path. The remaining arguments are the compile's include
# directories. It depends on every X source it could import (STD_SRCS,
# XC_SRCS), so a library edit rebuilds every program.
function(cx_x_program var src type ext)
    get_filename_component(name ${src} NAME_WE)
    set(elf ${BUILD_DIR}/${name}.elf)
    set(pkg ${PACKAGES_DIR}/${name}.${ext})
    set(incs "")
    foreach(dir ${ARGN})
        list(APPEND incs -I ${dir})
    endforeach()
    add_custom_command(
        OUTPUT ${elf}
        COMMAND ${CXK} compile ${src} ${elf} --target x86_64 ${incs}
        DEPENDS ${src} ${CXK} ${STD_SRCS} ${XC_SRCS}
        COMMENT "X   ${name}.xfxn -> ELF64"
    )
    cx_sign(sign ${pkg})
    add_custom_command(
        OUTPUT ${pkg}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGES_DIR}
        COMMAND ${CXK} build ${elf} ${pkg} --type ${type}
        ${sign}
        DEPENDS ${elf} ${CXK}
        COMMENT "Packaging ${name}.${ext} (CXEX, type ${type})"
    )
    set(${var} ${pkg} PARENT_SCOPE)
endfunction()
