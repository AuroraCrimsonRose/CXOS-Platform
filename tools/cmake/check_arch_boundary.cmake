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
# Outside kernel/arch/ and kernel/test/ a file may not name the direct map
# or the physical window (DIRECT_MAP_*, PHYS_WINDOW_*), nor the
# architecture's half of memman/physmap.h (paging_phys_slot, paging_map_ram),
# except memman/physmap.c itself: physical memory is reached through
# phys_map, so a port without a direct map changes only the architecture
# (roadmap 1.9c). Device registers go through paging_map_phys.
#
# Outside kernel/arch/, kernel/platform/ and kernel/test/ a file may not
# include a platform's own headers, anything under platform/<name>/: generic
# code reaches the machine's fixed devices through kernel/platform/*.h. The
# architecture may (x86 interrupts arrive through the PC's 8259).
#
# In every file, kernel/arch/ too, user memory goes through the recoverable
# copies (roadmap 1.9e4): user_ptr() only in cpu/usercopy.c, the self-tests'
# ktest_umem() only in ktest*.c and cpu/usermode.c.
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
set(phys_re  "DIRECT_MAP_|PHYS_WINDOW_|paging_phys_slot|paging_map_ram")

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
    if(NOT rel STREQUAL "memman/physmap.c")
        list(APPEND rules phys)
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
            elseif(rule STREQUAL "phys")
                set(why "physical memory reached around memman/physmap.h")
            else()
                set(why "a platform's own header")
            endif()
            string(APPEND found "\n  kernel/${rel}: ${why}: ${h}")
        endforeach()
    endforeach()
endforeach()

# User memory (roadmap 1.9e4), checked in every file, kernel/arch/ too: no
# syscall dereferences a user address, since a sibling thread on another CPU
# can unmap it between a check and the read. user_ptr() lives only in
# cpu/usercopy.c, behind the recoverable copies; ktest_umem() is the
# self-tests' own view of their pages, allowed only in ktest*.c and
# cpu/usermode.c (which includes it below its last syscall).
set(uptr_re  "(^|[^A-Za-z0-9_])user_ptr[ \t]*[(]")
set(umem_re  "(^|[^A-Za-z0-9_])ktest_umem[ \t]*[(]")
set(found_umem "")
foreach(f ${sources})
    file(RELATIVE_PATH rel "${KDIR}" "${f}")
    get_filename_component(base "${f}" NAME)
    if(NOT rel STREQUAL "cpu/usercopy.c")
        file(STRINGS "${f}" hits REGEX "${uptr_re}")
        foreach(h ${hits})
            string(STRIP "${h}" h)
            string(APPEND found_umem "\n  kernel/${rel}: a user address as a pointer outside cpu/usercopy.c: ${h}")
        endforeach()
    endif()
    if(NOT base MATCHES "^ktest" AND NOT rel STREQUAL "cpu/usermode.c" AND NOT rel STREQUAL "diag/ktest_umem.h")
        file(STRINGS "${f}" hits REGEX "${umem_re}")
        foreach(h ${hits})
            string(STRIP "${h}" h)
            string(APPEND found_umem "\n  kernel/${rel}: the self-tests' user-page view outside them: ${h}")
        endforeach()
    endif()
endforeach()
if(found_umem)
    message(FATAL_ERROR "User memory read or written where it lies (CX_ROADMAP.md 1.9e4):${found_umem}\n"
                        "Go through user_copy_in, user_copy_out or user_copy_str (cpu/usermode.h).")
endif()

if(found)
    message(FATAL_ERROR "CPU- or platform-specific code in a generic file (CX_ROADMAP.md step 3):${found}\n"
                        "Put it behind an interface in kernel/arch/*.h or kernel/platform/*.h, "
                        "implemented in kernel/arch/<arch>/ or kernel/platform/<platform>/.")
endif()

if(STAMP)
    file(WRITE "${STAMP}" "")
endif()
