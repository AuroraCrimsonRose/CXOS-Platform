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

# ---- code-signing + executive + unified-disk pipeline ----
option(SIGN "sign CXEX artifacts (.xkex/.xoex) with the kernel key" OFF)

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

# 4. Set the OS paths using the true SRC_DIR we just fixed in CMakeLists!
set(EXEC_C    "${SRC_DIR}/os/executive/executive.c")
set(EXEC_LD   "${SRC_DIR}/os/executive/executive.ld")
set(EXEC_ELF  ${BUILD_DIR}/executive.elf)
set(EXEC_XOEX ${PACKAGES_DIR}/executive.xoex)

set(CXK_DISK  ${IMAGES_DIR}/cxk_disk.img)

# per-artifact sign step
if(SIGN)
    if(NOT EXISTS ${SIGN_SK})
        message(FATAL_ERROR "SIGN=ON but signing key missing: ${SIGN_SK}\n  run: tools/cxk.exe keygen tools/kernel")
    endif()
    # One variable per ARTIFACT, not per kind: each expands to a command naming
    # the exact file it signs. Reusing ASIGN for a second program signs hi.xuex
    # twice and leaves the other one unsigned, which shows up as a signature
    # refusal at boot rather than as a build error.
    set(KSIGN COMMAND ${CXK} sign ${KERNEL_XKEX} ${SIGN_SK} ${SIGN_PK})
    set(XSIGN COMMAND ${CXK} sign ${EXEC_XOEX}   ${SIGN_SK} ${SIGN_PK})
    set(ASIGN COMMAND ${CXK} sign ${DISK_APP_XCEX} ${SIGN_SK} ${SIGN_PK})
    set(VSIGN COMMAND ${CXK} sign ${SUPERVISOR_XCEX} ${SIGN_SK} ${SIGN_PK})
    set(TSIGN COMMAND ${CXK} sign ${TOKDUMP_XUEX} ${SIGN_SK} ${SIGN_PK})
    set(PSIGN COMMAND ${CXK} sign ${ASTDUMP_XUEX} ${SIGN_SK} ${SIGN_PK})
    set(MSIGN COMMAND ${CXK} sign ${SEMADUMP_XUEX} ${SIGN_SK} ${SIGN_PK})
    set(CSIGN COMMAND ${CXK} sign ${XC_XUEX} ${SIGN_SK} ${SIGN_PK})
elseif(DEV_UNSIGNED)
    # A development kernel runs these unsigned, so the release build's "WILL
    # REFUSE" warnings below would be false here - and a false warning sends
    # someone chasing a problem that does not exist. Same rule on the text:
    # no parentheses, no semicolons.
    set(KSIGN COMMAND ${CMAKE_COMMAND} -E echo "  kernel.xkex UNSIGNED - development kernel, never ship")
    set(XSIGN COMMAND ${CMAKE_COMMAND} -E echo "  executive.xoex UNSIGNED - runs on this development kernel only")
    set(ASIGN COMMAND ${CMAKE_COMMAND} -E echo "  hi.xuex UNSIGNED - runs on this development kernel only")
    set(VSIGN COMMAND ${CMAKE_COMMAND} -E echo "  supervisor.xsex UNSIGNED - runs on this development kernel only")
    set(TSIGN COMMAND ${CMAKE_COMMAND} -E echo "  tokdump.xuex UNSIGNED - runs on this development kernel only")
    set(PSIGN COMMAND ${CMAKE_COMMAND} -E echo "  astdump.xuex UNSIGNED - runs on this development kernel only")
    set(MSIGN COMMAND ${CMAKE_COMMAND} -E echo "  semadump.xuex UNSIGNED - runs on this development kernel only")
    set(CSIGN COMMAND ${CMAKE_COMMAND} -E echo "  xc.xuex UNSIGNED - runs on this development kernel only")
else()
    # No parentheses OR semicolons in these messages: ${KSIGN}/${XSIGN} expand into a
    # custom-command line, and under /bin/sh unquoted parens are a syntax error while a
    # semicolon silently splits the command - the text after it just disappears. Both are
    # harmless under cmd.exe, so the restriction costs nothing on Windows.
    set(KSIGN COMMAND ${CMAKE_COMMAND} -E echo "  kernel.xkex UNSIGNED - configure -DSIGN=ON to sign")
    set(XSIGN COMMAND ${CMAKE_COMMAND} -E echo "  executive.xoex UNSIGNED - the kernel WILL REFUSE to launch it - configure -DSIGN=ON")
    set(ASIGN COMMAND ${CMAKE_COMMAND} -E echo "  hi.xuex UNSIGNED - exec_path WILL REFUSE it - configure -DSIGN=ON")
    set(VSIGN COMMAND ${CMAKE_COMMAND} -E echo "  supervisor.xsex UNSIGNED - the executive WILL REFUSE it - configure -DSIGN=ON")
    set(TSIGN COMMAND ${CMAKE_COMMAND} -E echo "  tokdump.xuex UNSIGNED - exec_path WILL REFUSE it - configure -DSIGN=ON")
    set(PSIGN COMMAND ${CMAKE_COMMAND} -E echo "  astdump.xuex UNSIGNED - exec_path WILL REFUSE it - configure -DSIGN=ON")
    set(MSIGN COMMAND ${CMAKE_COMMAND} -E echo "  semadump.xuex UNSIGNED - exec_path WILL REFUSE it - configure -DSIGN=ON")
    set(CSIGN COMMAND ${CMAKE_COMMAND} -E echo "  xc.xuex UNSIGNED - exec_path WILL REFUSE it - configure -DSIGN=ON")
endif()