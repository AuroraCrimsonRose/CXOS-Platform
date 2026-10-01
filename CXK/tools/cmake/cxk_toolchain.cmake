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

# A signed kernel that runs unsigned code is the one artifact this must never
# produce: it looks official, and it trusts nothing. Refuse the combination at
# configure time rather than hope nobody builds it.
if(SIGN AND DEV_UNSIGNED)
    message(FATAL_ERROR "SIGN and DEV_UNSIGNED are mutually exclusive: together they would "
                        "produce a signed kernel that runs unsigned code. Pick one.")
endif()

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