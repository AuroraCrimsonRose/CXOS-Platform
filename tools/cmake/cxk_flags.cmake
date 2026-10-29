# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
# ============================================================================
#  CXK build-time flags  -  EDIT HERE, not CMakeLists.
# ============================================================================
#  Each entry below is NAME=VALUE. CMakeLists turns every one into -DNAME=VALUE
#  and passes it to BOTH the kernel C compiler (clang) AND the assembler (nasm),
#  so the kernel and the bootloader can never disagree about a flag.
#
#  The kernel's config.h and the boot/*.asm files keep their own
#  #ifndef / %ifndef defaults purely as a FALLBACK for builds that don't pass
#  -D (e.g. a bare host-compile check). When you build through CMake, the value
#  here WINS.
#
#  Add a flag      -> add a line.
#  Flip a flag     -> change its value.
#  No CMakeLists.txt edits either way.
# ============================================================================

set(CXK_FLAGS
    CXK_ENABLE_FB=1         # 1 = bootloader sets a VBE LFB mode + kernel uses the framebuffer console
                            #     0 = force VGA text mode end-to-end (bootloader never touches video;
                            #         use this to keep early-boot output visible when debugging a fault)
    CXK_ENABLE_SERIAL=1     # 1 = tee all console output to a 16550 UART on COM1 at 115200 8N1.
                            #     Costs nothing when no UART is present (the driver probes with a
                            #     loopback test and goes quiet), and makes a boot readable as text
                            #     in BOTH video modes: qemu ... -serial file:boot.log
    CXK_ALLOW_DISK_WRITE=1  # 1 = DEV build: may format / write a scratch disk. DANGER on bare metal.
                            #     0 = read-only/never-format (safe default for sharing)
    CXK_AUTO_INSTALL=0      # 0 = write ONLY to CXOS's own disk (the stick it booted from); every
                            #     other disk is refused in the disk layer. Safe on any machine.
                            # 1 = may claim other disks: a second disk with no CXFS is formatted.
    CXK_KTEST_STACK_OVERFLOW=0  # 1 = after the self-tests, run a thread that recurses until it
                            #     overflows its kernel stack. The boot MUST end in a red panic naming
                            #     that thread - which proves the guard page and the double-fault task
                            #     work. A reset or a hang instead means they do not. Test builds only.
                            # 2 = the same on thread 0's own stack; the panic must name "main".
    CXK_TEST_HOSTILE_BIOS=0 # 1 = stage 2 plays a hostile BIOS: after every int 13h it leaves FS and
                            #     GS with 64 KB limits. The boot MUST still pass, because stage 2
                            #     re-enters unreal mode after each call. Test builds only.
)