# Transitional code register

Both engineering reviews ask the same thing (kernel §15, DevKit §14): classify
code that is on its way somewhere, so a temporary path does not quietly become
an undocumented second implementation. This file is that classification. It is
a register, not a history: an entry leaves when its code does.

**The classes.** Every entry is exactly one of them.

| Class | Meaning |
|---|---|
| **Planned replacement** | The supported path today, with a named successor and a named trigger for the switch. |
| **Compatibility** | Kept so something older keeps working. |
| **Test-only** | Exists for verification, whatever binary it lands in. |
| **Declared, not live** | A name, flag or function the code defines and nothing acts on. |
| **Obsolete** | Its replacement is proven. It gets deleted, not registered. |

**The rule.** Transitional code carries a comment at the site saying what
replaces it and when. The register is the index of those comments. A sweep
re-reads each entry against the code. When the trigger has fired, the entry is
either acted on or rewritten to say why not.

"Legacy" in this codebase usually names **hardware**, not code: legacy PIO ATA,
the 8259 IRQ range, PCI configuration mechanism #1, e1000 legacy descriptors,
EHCI's legacy-support capability. Those are active drivers for real machine
interfaces and are not transitional.

---

## Removed in the 2026-10-16 sweep

| What | Why it could go |
|---|---|
| `boot/uefi/secureboot.bat` | D2's last script. Its own retirement condition (`boot/uefi/README.md` gives the `cxk` sequence) was met. The sequence was then run from scratch and all three Secure Boot boots passed before the script was deleted. |
| The pre-merge layout fallbacks: `CheckCommand.ResolveRoot`'s "CMakeLists at the root" candidate, and the `CXK/os` candidates in `HostBuild.FindOsDir`, `LangRunTests` and `XDataDiffTests` | Kept for "a tree that still nests it". After D6 there is one repository, and nothing nests it. |
| Comments citing `mkcxes.py`, `signcxex.py`, `makekeys.py` and `mkdisk.py` as the reference for a layout | The scripts were deleted under D1/D2. Nine sites still named them as the authority for byte layouts. They now name `CXEXWriter`, `CXSigner`, `cxk keygen` and `XBPTImageWriter`. "Replaces X.py" comments stay, because those are history, not authority. |
| `kernel.asm`'s "v5" | The internal generation counter `versions.json` retired. |
| `sched.c`'s "per-process esp0 is wired in step 2b" | Step 2b was done long ago. `thread_create_process` gives every process its own esp0. The comment sat in `thread_create`, which builds kernel-only threads, and read as if esp0 were still to come. Rewritten, and the fallback it described made true (`PROCESS_MODEL.md` §8). |
| `build.sh`'s "legacy Python suites" | They have been the C# Differential suites since D1. |
| `CXEX_FLAG_KERNEL_PRIV` (header bit 3) | Retired 2026-10-16, on Aurora's decision. Every kernel image set it and nothing read it, and a privilege bit an image sets on itself invites a future reader to trust it (DevKit security §4). Writers no longer set it and readers ignore it, so there is no format bump. The bit is reserved and never reused. The kernel image's bytes changed. `CompatDeclarationTests` fails if any image type sets it again. |
| CXFS's on-disk advisory locks: `cxfs_lock`, `cxfs_unlock`, `cxfs_is_locked`, `CXFS_E_LOCKED`, `CXFS_FEAT_LOCKING`, and the entry fields `lock_state` / `lock_owner_pid` | Replaced 2026-10-16, on Aurora's decision, by advisory locks held by the open file in memory and released when its handle goes (`FILE_OP_LOCK`/`FILE_OP_UNLOCK`). That is SerenityOS's model, and Haiku, XNU and NT all agree that locks live off disk and end at close. The old ones could not be taken by any syscall. They were owned by a reused pid and kept across reboots, yet still **enforced** on writes and unlinks, so a crafted volume could refuse writes to its own entries. The entry bytes and feature bit 2 are reserved, not reused. The ktest "advisory locks follow the open file" covers the replacement, and it sets the old bytes to a live owner to show they are ignored. |

## Planned replacement

| Where | Today | Replaced by | Trigger |
|---|---|---|---|
| `devkit/CXEX.Lang` | The C# X compiler, which is the oracle `os/xc` is tested against | `os/xc`, self-hosted | `os/xc` matches it on every differential suite and the self-hosting fixed point holds, which is already true. Retiring the oracle is a decision, not a test result. |
| `kernel/arch/x86/fpu.c` | FP and SSE left disabled | `fxsave`/`fxrstor` or lazy switching in `context_switch` | Before anything enables FP. The file explains why enabling it today would corrupt state. |
| `kernel/drivers/input/keyboard.c` `kwaiter` | One blocked reader | A wait queue | A second process that reads the keyboard |
| `kernel/drivers/net/netif.h` | Static IP configuration | DHCP | A network that is not QEMU's user-mode NAT |
| `kernel/lib/format/cxex_loadk.c` `ctx = 0` | Loads into the current address space | An explicit target space | Loading into a process other than the caller |
| `kernel/drivers/video/fb.c` | Write-back caching on the framebuffer | Write-combining (PAT) | Framebuffer throughput matters |
| `kernel/cpu/exec.c` unverified-image policy | Always refuses | An administrator's choice | User accounts exist |
| `/Shared/Config` at 0777 | Any program with `GRANT_DISK` can write any program's settings | `/User/<name>` at 0700 | Users exist (`CX_FILESYSTEM_LAYOUT.md` rule 6) |
| `kernel/arch/x86_64/boot64.c` | The x86-64 kernel's whole run: GDT, TSS, IDT, the handover, paging and the page allocator, 27 bring-up self-tests, halt | `kmain` and `ktest_run`, built for x86-64 | Roadmap step 5d, when interrupts, the timer and the scheduler run on x86-64 |
| `kernel/arch/x86_64/early_con.c` | COM1, polled, for the x86-64 kernel | `klog` over `drivers/char/serial.c` | The same: `klog` running on x86-64 |
| `kernel/arch/x86_64/idt.c` `kpanic` | The banner, the reason and a halt, on COM1 | The panic screen, its stack trace and the crash log (`kernel/diag/`) | Those running on x86-64 |
| `kernel/memman/pmm.c` | The 32-bit kernel's bitmap page allocator; its 4 MiB boot window cannot hold a page database | `memman/pagedb.c`, the page database and buddy allocator x86-64 uses | Roadmap step 6: the 32-bit kernel is deleted |
| Drivers' DMA memory | e1000 and RTL8169 use fixed physical regions (`0x700000`, `0x730000`; `pmm.c` reserves `0x500000`-`0x800000`); AHCI programs 32-bit addresses and refuses a frame above 4 GB | `pmm_alloc_pages_low`, the zone below 4 GB, for every DMA allocation | The drivers running on x86-64 |

| The x86-64 kernel image as a plain ELF | Stage 2 loads it unsigned and unchecked, beside the 32-bit CXEX path | CXEX for x86-64, the signature check in the loader | `CX_BOOT.md` section 4 item 3 |

## Test-only

| Where | Note |
|---|---|
| `kernel/ktest*.c` (`ktest_run`) | Ships in every kernel and runs at every boot. Read-only. This is established practice here. |
| `setcheck.xuex` | Ships in the image and **writes** to `/Shared/Config` at every boot. A cut mid-test leaves `kttest.*` behind. Aurora's call before the next release: gate it behind a build flag, or keep it. |

## Declared, not live

| Name | State | Recommendation |
|---|---|---|
| `GRANT_MEM` | Defined in the ABI and in `caps.h`, enforced nowhere. `SYS_MEM_OP` is unprivileged by design. Marked reserved at both sites. | Keep it reserved: the bit is spoken for, and reusing it would be a silent ABI change. |
| `kernel/arch/x86/fpu.c`, `kernel/lib/math/kmath.c` | Not in the build at all, deliberately. `fpu.c` documents why: FP needs FPU state saved on context switch before anything can use it. `CMakeLists.txt` keeps x87 enabled so wiring them in is a source-list change. Its comment used to say `kmath.c` "genuinely uses double" in the kernel, which it does not. Since step 3e `kmath.c` is plain C (its square root was the SSE instruction) and host-tested (`test_kmath.c`), which found sine and cosine 2e-5 out at the ends of their range rather than the 1e-9 claimed; fixed. | Keep until FP is wanted; the trigger is in the "Planned replacement" table. |
| `CXAuthChain` (DevKit) | **Active, not dead.** Rewritten 2026-10-09 to the right model (the level derives from which key signed) and covered by `TrustAuthorityTests`. It is the DevKit's advisory mirror of `keyvault_trust_of`, independent by design (security §12.5). No CLI command surfaces it yet. | Keep. A `cxk inspect` line saying "the kernel will admit this as Publisher" is its natural first caller. |

## Unused surface: not transitional, but recorded

The graph and a grep pass agree that about 60 kernel functions have no caller.
Almost all of them are deliberate API: driver identity getters (`ata_model`,
`ahci_is_ssd`, `e1000_stats`), the string library, `kmath`, console field
helpers, and heap and PMM statistics. Some are features with no front end yet
(`speaker_boot_chime`, `klogo_draw_centered`, `acpi_sleep_s1`). None is a second
implementation of anything, so none is transitional.

Three are worth a look because an unused *reporting* function often means
errors are reported less well than they could be: `cxex_verify_strerror`,
`disk_err_str` and `disk_driver_name`. And `irq_mask`, `irq_unmask` and
`irq_uninstall_handler` have no caller, so no driver ever releases an IRQ.

Keep `memcmp`, `memmove` and `memcpy` whatever a call graph says: clang emits
calls to them on its own, for struct copies and comparisons.

## Open finding: the CI image's Python

`tools/ci/Dockerfile` installs `uv` and Python 3.12 "for the boot harness". The
boot harness (`bootverify.py`, `serialboot.ps1`) lives in `E:\Sandbox\scratch`,
outside the repository, and `tools/ci/build.sh` never calls it. The `uv` layer
is also the only unpinned image reference in the repository
(`ghcr.io/astral-sh/uv:latest`), which sits badly with the pinned toolchain
(`versions.json`, `toolchain`). Either bring the harness into the repository
and pin `uv` to a version, or drop the layer. Not changed in the sweep, because
the image may be used with the harness mounted in.
