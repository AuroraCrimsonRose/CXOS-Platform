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
    CXK_ALLOW_DISK_WRITE=1  # 1 = DEV build: may format / write a scratch disk. DANGER on bare metal.
                            #     0 = read-only/never-format (safe default for sharing)
    CXK_KTEST_STACK_OVERFLOW=0  # 1 = after the self-tests, run a thread that recurses until it
                            #     overflows its kernel stack. The boot MUST end in a red panic naming
                            #     that thread - which proves the guard page and the double-fault task
                            #     work. A reset or a hang instead means they do not. Test builds only.
                            # 2 = the same on thread 0's own stack; the panic must name "main".
)