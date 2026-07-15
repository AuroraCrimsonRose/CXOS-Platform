# ============================================================================
# cxk_toolchain.cmake  -  CXEX toolchain via the cxk CLI (replaces Python)
# Aurora Tejeda / CATX SYSTEMS LLC
#
# Drop-in replacement for the old Python toolchain variables in CMakeLists.txt.
# Every step that used to shell to a .py now calls tools/cxk.exe, whose command
# arguments are identical to the scripts they replaced:
#
#   mkcxes.py  IN OUT --type X    ->  cxk build   IN OUT --type X
#   signcxex.py FILE SK PK        ->  cxk sign     FILE SK PK
#   mkdisk.py  --out ... --stage  ->  cxk image    --out ... --stage
#   pad_boot.ps1 -Boot ...        ->  cxk raw-image --boot ...
#
# To adopt: replace the old "set(VENV_PY ...)"/"set(MKCXES ...)"/... block and
# the KSIGN/XSIGN if(SIGN) block in CMakeLists.txt with an include of this file.
# ============================================================================

# The single toolchain binary. Committed? No - it's a build artifact (gitignored);
# each machine publishes it from the DevKit. Override with -DCXK=path if it lives
# elsewhere.
if(NOT DEFINED CXK)
    set(CXK ${CXK_ROOT}/tools/cxk.exe)
endif()

if(NOT EXISTS ${CXK})
    message(FATAL_ERROR
        "cxk toolchain not found: ${CXK}\n"
        "  Publish it from the DevKit:\n"
        "    dotnet publish CXEX.CLI/CXEX.CLI.csproj -c Release -r win-x64 --self-contained "
        "-p:PublishSingleFile=true -o <out>\n"
        "  then copy cxk.exe into tools/  (or pass -DCXK=<path>).")
endif()

# ---- code-signing + executive + unified-disk pipeline ----
option(SIGN "sign CXEX artifacts (.xkex/.xoex) with the kernel key" OFF)
set(SIGN_SK   ${CXK_ROOT}/tools/kernel.xksk)   # was tools/CXEX_Compiler/kernel.xksk
set(SIGN_PK   ${CXK_ROOT}/tools/kernel.xkpk)

# the system executive (.xoex): os/executive/executive.c -> ELF -> CXEX (type os)
set(EXEC_C    ${CXK_ROOT}/os/executive/executive.c)
set(EXEC_LD   ${CXK_ROOT}/os/executive/executive.ld)
set(EXEC_ELF  ${BUILD_DIR}/executive.elf)
set(EXEC_XOEX ${PACKAGES_DIR}/executive.xoex)

# the unified XBPT disk (boot + stage + system on one disk)
set(CXK_DISK  ${IMAGES_DIR}/cxk_disk.img)

# per-artifact sign step: real `cxk sign` when -DSIGN=ON, else a harmless notice.
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
