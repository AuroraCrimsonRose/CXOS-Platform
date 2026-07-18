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
    set(KSIGN COMMAND ${CXK} sign ${KERNEL_XKEX} ${SIGN_SK} ${SIGN_PK})
    set(XSIGN COMMAND ${CXK} sign ${EXEC_XOEX}   ${SIGN_SK} ${SIGN_PK})
else()
    set(KSIGN COMMAND ${CMAKE_COMMAND} -E echo "  kernel.xkex UNSIGNED (configure -DSIGN=ON to sign)")
    set(XSIGN COMMAND ${CMAKE_COMMAND} -E echo "  executive.xoex UNSIGNED (configure -DSIGN=ON to sign)")
endif()