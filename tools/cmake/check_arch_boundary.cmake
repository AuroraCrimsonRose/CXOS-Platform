# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
#
# The architecture boundary (docs/planning/CX_ROADMAP.md, Phase 0 step 3):
# no file outside kernel/arch/ contains code for one CPU, and none outside
# kernel/arch/ and kernel/platform/ names one platform's devices. Run by the
# kernel build before it links, so a violation stops the build:
#
#   cmake -DKDIR=<repo>/kernel -DSTAMP=<file> -P check_arch_boundary.cmake
#
# Outside kernel/arch/ (and kernel/test/, whose host tests test arch code
# directly) a file may not:
#   - contain inline assembly (asm, __asm, __asm__), or be assembly itself
#     (.asm, .S, .s), which is also how port I/O and every privileged
#     instruction would arrive;
#   - use compiler intrinsics for one CPU (__builtin_ia32_*, <cpuid.h>,
#     <*intrin.h>);
#   - include an architecture's private headers: anything under arch/<name>/.
#     The headers directly in kernel/arch/ are the interface every
#     architecture implements, and are what generic code includes.
#
# Outside kernel/arch/, kernel/platform/ and kernel/test/ a file may not
# include a platform's own headers, anything under platform/<name>/: generic
# code reaches the machine's fixed devices through kernel/platform/*.h. The
# architecture may (x86 interrupts arrive through the PC's 8259).
#
# A match is reported with its file and the offending line. Text in comments
# counts too; that is rare enough to be reworded when it happens.

if(NOT KDIR)
    message(FATAL_ERROR "check_arch_boundary: KDIR not set")
endif()

set(asm_re   "(^|[^A-Za-z0-9_.])(__asm__|__asm|asm)[ \t]*(volatile|__volatile__|goto|inline)?[ \t]*[(]")
set(intr_re  "__builtin_ia32_|#[ \t]*include[ \t]*<[A-Za-z0-9_]*(intrin|cpuid)[.]h>")
set(incl_re  "#[ \t]*include[ \t]*[\"<][^\">]*arch/[A-Za-z0-9_]+/")
set(plat_re  "#[ \t]*include[ \t]*[\"<][^\">]*platform/[A-Za-z0-9_]+/")

file(GLOB_RECURSE sources LIST_DIRECTORIES false "${KDIR}/*.c" "${KDIR}/*.h")
file(GLOB_RECURSE asmfiles LIST_DIRECTORIES false "${KDIR}/*.asm" "${KDIR}/*.S" "${KDIR}/*.s")

set(found "")
foreach(f ${asmfiles})
    file(RELATIVE_PATH rel "${KDIR}" "${f}")
    if(NOT rel MATCHES "^arch/")
        string(APPEND found "\n  kernel/${rel}: assembly source outside kernel/arch/")
    endif()
endforeach()

foreach(f ${sources})
    file(RELATIVE_PATH rel "${KDIR}" "${f}")
    if(rel MATCHES "^(arch|test)/")
        continue()
    endif()
    set(rules asm intr incl)
    if(NOT rel MATCHES "^platform/")
        list(APPEND rules plat)
    endif()
    foreach(rule ${rules})
        file(STRINGS "${f}" hits REGEX "${${rule}_re}")
        foreach(h ${hits})
            string(STRIP "${h}" h)
            if(rule STREQUAL "asm")
                set(why "inline assembly")
            elseif(rule STREQUAL "intr")
                set(why "a CPU-specific intrinsic")
            elseif(rule STREQUAL "incl")
                set(why "an architecture's private header")
            else()
                set(why "a platform's own header")
            endif()
            string(APPEND found "\n  kernel/${rel}: ${why}: ${h}")
        endforeach()
    endforeach()
endforeach()

if(found)
    message(FATAL_ERROR "CPU- or platform-specific code in a generic file (CX_ROADMAP.md step 3):${found}\n"
                        "Put it behind an interface in kernel/arch/*.h or kernel/platform/*.h, "
                        "implemented in kernel/arch/<arch>/ or kernel/platform/<platform>/.")
endif()

if(STAMP)
    file(WRITE "${STAMP}" "")
endif()
