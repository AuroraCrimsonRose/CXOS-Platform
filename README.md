# CXOS Platform

Everything that makes CXOS, in one repository: **CXK**, the kernel; the **X
userland**, including the X compiler written in X; the boot chain; and the **CX
DevKit**, the host toolchain (the `cxk` CLI, the C# X compiler, signing,
imaging).

## Status

**This is the x86-64 line** (`x86_64-DEV`), and x86-64 is the only kernel,
image and compiler back end it builds. The first real hardware is a BIOS-only
AMD machine
([`docs/hardware/CX_TARGET_HARDWARE.md`](docs/hardware/CX_TARGET_HARDWARE.md)).
The retired x86-32 system is kept as a reference on the `x86_32_DEV` and
`x86_32_RELEASE` branches.

Phase 0 of the [roadmap](docs/planning/CX_ROADMAP.md) is done: the kernel
object model, processes separate from threads, the architecture layer, the
CXBI hand-over and crash log, the 64-bit kernel booting from the BIOS loader,
and X for x86-64. Phase 1 has one step left, 1.9f (the race tests as an SMP
stress suite on 8 CPUs, Hyper-V, then the rig); so far it has brought:

- process handles (wait, stop, kill, inspect), jobs, and detailed exit
  reasons;
- one wait queue for everything, and timers as handles (LAPIC timer and TSC);
- IPC with queued calls, reply tokens, badges and handle transfer; pipes, and
  handles passed explicitly at spawn; the shell running from disk;
- user threads, futexes and thread-local storage;
- input as a granted right, and a kernel RNG (ChaCha20, Fortuna-style pools);
- a hardware report diffed across QEMU, Hyper-V and the rig (`cxk hw diff`);
- SMP: a ranked lock order with a checker, per-CPU data, the APs started from
  the MADT, TLB shootdown, the scheduler and ring-3 threads on every CPU, and
  every syscall reaching user memory only through copies.

Carried over from x86-32 and checked by every full boot: signed executables
verified before they run, ring-3 processes with their own address spaces,
capabilities attenuated at spawn, CXFS with multiple volumes, an X userland
with a shell, services run by a supervisor, and kernel configuration in X
Data. The X compiler written in X compiles itself to a fixed point. The
shell's windowed GUI (`gui`) came over too; no boot test exercises it.

Not yet: Phase 2's display server and driver model, the new storage layout of
Phase 3, UEFI boot, and a run on the target rig. How correctness is checked
today, and what is still missing (fuzzing, model checking, proofs), is in
[`docs/safety/CX_SAFETY.md`](docs/safety/CX_SAFETY.md) §1.

**Security:** under active hardening, not yet hardened. The plan is
[`docs/security/CX_SECURITY.md`](docs/security/CX_SECURITY.md); what was found
and fixed on x86-32 is in
[`docs/planning/HARDENING_PLAN.md`](docs/planning/HARDENING_PLAN.md).

## Layout

| Directory | What it holds |
|---|---|
| `abi/` | The syscall and boot ABI both sides compile against |
| `boot/` | The BIOS boot chain (NASM); `uefi/` stub |
| `kernel/` | CXK; `ktest*.c` are the boot self-tests |
| `os/` | X userland: `apps/`, `std/`, `xc/` (the compiler in X), `executive/`, `config/`, `services/` |
| `devkit/` | The C# toolchain and its tests: see [`devkit/README.md`](devkit/README.md) |
| `editors/` | X language support for VS Code |
| `vendor/` | Third-party components, unmodified: see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) |
| `assets/` | Branding |
| `tools/` | The CMake build (`cmake/`), the CI container (`ci/`), the platform public key |
| `docs/` | All documentation: start at [`docs/README.md`](docs/README.md) |

## Building

Requirements, on every host:

- **clang 23 or newer, and ld.lld.** The kernel is built with
  `clang --target=x86_64-elf`; no cross-GCC. Clang 18 through 22 link through
  `gcc` for this target and do not work. `tools/ci/Dockerfile` pins a
  known-good toolchain.
- **CMake and Ninja.** No MSVC, no Developer Command Prompt.
- **NASM** for the boot chain and the kernel's assembly.
- **QEMU**, the only emulator CXOS targets.
- **.NET 10 SDK** for the DevKit.

`tools/cxk.exe` is the DevKit's CLI, and it is not committed. Take it from a
[release](https://github.com/AuroraCrimsonRose/CXOS-Platform/releases), or
publish it from `devkit/` and copy it to `tools/`:

```
dotnet publish devkit/CXEX.CLI -c Release -r win-x64 -p:SelfContained=true -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
```

Republish whenever the DevKit changes; `cxk check-abi` names a stale one.

| Task | Command |
|---|---|
| Build, signed with a test key | `cxk keygen tools/test` once, then `cxk os build --key test` |
| Build a development kernel (runs unsigned programs; never ship it) | `cxk os build --dev` |
| Run | `cxk run dist/CXK_x86_64/images/cxk_x86_64.img` |
| Boot and check the self-tests | `cxk verify-boot dist/CXK_x86_64/images/cxk_x86_64.img --full -t 600` (4 CPUs; `--json` gives each test's result) |
| The same on one CPU | add `--smp 1` |
| The same on a full CPU model | add `--cpu max` (no minimal x86-64 model passes yet: with `--cpu qemu64,-apic` two APIC self-tests fail) |
| The same under Hyper-V (Gen 1, AMI BIOS) | `cxk verify-hyperv dist/CXK_x86_64/images/cxk_x86_64.img` |
| The same booted from a USB stick, as the rig boots | add `--usb xhci` (or `ehci`, `ohci`) |
| A machine's own drive with another CXOS on it, ahead of the image, comes out unchanged | add `--foreign-disk` |
| A deliberate panic, and its crash log recovered by the next boot | `cxk verify-crashlog dist/CXK_x86_64/images/cxk_x86_64.img` (fails on x86-64 until its panic path writes the stack trace; the cross-boot seed check is `cxk verify-seed`) |
| The same on a USB stick, the panic landing mid-transfer | add `--usb xhci` (or `ehci`, `ohci`) |
| Read the crash log a panic left on a disk image or stick | `cxk rig crashlog <image or disk5>` (a disk needs an administrator prompt) |
| Read the system log the last boot saved (USB summary, supervisor output) | `cxk rig bootlog disk5 -o bootlog.txt` (administrator prompt) |
| Find the stick's disk number | `cxk rig disks` |
| Copy a stick's CXOS part to an image | `cxk rig dump disk5 out.img` |
| A machine's hardware report, and two compared | `cxk hw show`, `cxk hw diff` (a stick's saved report: `cxk rig hwreport`) |
| Host tests | `dotnet test devkit/CXEX.slnx -c Release` |
| Everything: the build, both boots, host tests, the container, one summary | `cxk test all` (about 30 minutes) |
| A step's sabotage matrix: each check broken, rebuilt, required red | `cxk test sabotage tests/sabotage/<step>.sab` (`--dry-run` first; `--all`) |
| CI, by hand | `tools/ci/Dockerfile` and `tools/ci/build.sh` |

A release kernel refuses unsigned programs, so an unsigned release build boots
to the kernel and stops. A development kernel says so at every boot.

## Documentation

[`docs/README.md`](docs/README.md) indexes everything. The plan is
[`docs/planning/CX_ROADMAP.md`](docs/planning/CX_ROADMAP.md); the documents
describing the inherited x86-32 system are frozen in
[`docs/x86_32/`](docs/x86_32/README.md).

## Licence

CXOS is source-available, not open source. It is licensed in two tiers, and which
one applies depends on where a file lives.

| | |
|---|---|
| The operating system: `boot/`, `kernel/`, `os/executive/`, `os/services/`, `os/apps/`, `os/config/` | [PolyForm Noncommercial 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt) |
| The SDK and tooling: `abi/`, `os/std/`, `os/xc/`, `devkit/`, `editors/`, `tools/`, `docs/` | [MIT](LICENSES/MIT.txt) |

Every source file carries an `SPDX-License-Identifier` header saying which, and
that header is what governs. Third-party components under `vendor/` keep their
own licences ([THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)).

**Applications you write for CXOS are independent works.** Compiling against the
ABI headers, linking the X standard library, making system calls and shipping as
CXEX do not make your program a derivative of the kernel. Licence and sell it
however you like.

Full detail, including the application exemption in its exact wording, is in
[LICENSE.md](LICENSE.md). Security policy is in [SECURITY.md](SECURITY.md).

## Credits

CXOS is written by **Aurora Tejeda** (CATX Systems). The kernel, the X language
and its two compilers, CXFS, the CXEX format and the boot chain are her work.

**Claude** (Anthropic) has contributed as a co-author since 2026-09-26, working
on the security hardening of the CXEX loader, the move to a clang/ld.lld/Ninja
toolchain, the version registry, the licensing and provenance of this
repository, and the x86-64 design documents. Those commits carry a
`Co-Authored-By` trailer, which is the honest boundary: the assistance is real
and it is recorded, and the project is Aurora's.

The console font is Terminus Font by Dimitar Toshkov Zhekov (OFL-1.1). See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

---

© Aurora Tejeda / CATX Systems
