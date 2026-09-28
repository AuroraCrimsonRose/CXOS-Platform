# ============================================================================
# cxk_toolchain.cmake  -  CXEX toolchain via the cxk CLI
# ============================================================================

# 1. Grab the tools folder path dynamically relative to THIS script
# This file is in:  CXK/tools/cmake
# One folder up:    CXK/tools
get_filename_component(CXK_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# 2. Set the CXK executable path (CXK/tools/cxk.exe)
if(NOT DEFINED CXK)
    set(CXK "${CXK_TOOLS_DIR}/cxk.exe")
endif()

# ---- code-signing + executive + unified-disk pipeline ----
option(SIGN "sign CXEX artifacts (.xkex/.xoex) with the kernel key" OFF)

# 3. Set keys relative to the tools directory (CXK/tools/...)
set(SIGN_SK   "${CXK_TOOLS_DIR}/kernel.xksk")
set(SIGN_PK   "${CXK_TOOLS_DIR}/kernel.xkpk")

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
    # the exact file it signs. Reusing ASIGN for a second .xcex signs hi.xcex
    # twice and leaves the other one unsigned, which shows up as a signature
    # refusal at boot rather than as a build error.
    set(KSIGN COMMAND ${CXK} sign ${KERNEL_XKEX} ${SIGN_SK} ${SIGN_PK})
    set(XSIGN COMMAND ${CXK} sign ${EXEC_XOEX}   ${SIGN_SK} ${SIGN_PK})
    set(ASIGN COMMAND ${CXK} sign ${DISK_APP_XCEX} ${SIGN_SK} ${SIGN_PK})
    set(VSIGN COMMAND ${CXK} sign ${SUPERVISOR_XCEX} ${SIGN_SK} ${SIGN_PK})
else()
    # No parentheses OR semicolons in these messages: ${KSIGN}/${XSIGN} expand into a
    # custom-command line, and under /bin/sh unquoted parens are a syntax error while a
    # semicolon silently splits the command - the text after it just disappears. Both are
    # harmless under cmd.exe, so the restriction costs nothing on Windows.
    set(KSIGN COMMAND ${CMAKE_COMMAND} -E echo "  kernel.xkex UNSIGNED - configure -DSIGN=ON to sign")
    set(XSIGN COMMAND ${CMAKE_COMMAND} -E echo "  executive.xoex UNSIGNED - the kernel WILL REFUSE to launch it - configure -DSIGN=ON")
    set(ASIGN COMMAND ${CMAKE_COMMAND} -E echo "  hi.xcex UNSIGNED - exec_path WILL REFUSE it - configure -DSIGN=ON")
    set(VSIGN COMMAND ${CMAKE_COMMAND} -E echo "  supervisor.xcex UNSIGNED - the executive WILL REFUSE it - configure -DSIGN=ON")
endif()