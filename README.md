# CXOS Platform

Everything that makes CXOS, in one repository: **CXK**, the kernel; the **X
userland**, including the X compiler written in X; the boot chain; and the **CX
DevKit**, the host toolchain (the `cxk` CLI, the C# X compiler, signing,
imaging).

## Status

**This is the x86-64 line** (`x86_64-DEV`). CXOS is moving to 64-bit, with a
BIOS-only AMD machine as the first real hardware
([`docs/hardware/CX_TARGET_HARDWARE.md`](docs/hardware/CX_TARGET_HARDWARE.md)).
The design work is done and Phase 0 of the
[roadmap](docs/planning/CX_ROADMAP.md) is starting: the kernel object model,
processes separate from threads, the architecture layer, then the 64-bit
kernel.

Until the 64-bit kernel boots, the tree still builds and boots the inherited
**x86-32** system, which is the baseline every change is tested against. It is
retired as a target: the `x86_32_DEV` and `x86_32_RELEASE` branches keep it as
a reference, and the 32-bit code leaves this branch once 64-bit passes the full
boot.

What the inherited system already does: signed executables verified before
they run, ring 3 processes with their own address spaces, capabilities
attenuated at spawn, CXFS with multiple volumes, an X userland with a shell and
a windowed GUI, services run by a supervisor, kernel configuration in X Data,
and an X compiler written in X that compiles itself to a fixed point.

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
  `clang --target=i686-elf`; no cross-GCC. Clang 18 through 22 do not work for
  this target. `tools/ci/Dockerfile` pins a known-good toolchain.
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
| Run | `cxk run dist/CXK_x86_32/images/cxk_disk.img` |
| Boot and check the self-tests | `cxk verify-boot dist/CXK_x86_32/images/cxk_disk.img --full` |
| The same on a minimal and a full CPU model | add `--cpu pentium2,-apic`, then `--cpu max` |
| The same booted from a USB stick, as the rig boots | add `--usb xhci` (or `ehci`, `ohci`) |
| A machine's own drive with another CXOS on it, ahead of the image, comes out unchanged | add `--foreign-disk` |
| A deliberate panic, and its crash log recovered by the next boot | `cxk verify-crashlog dist/CXK_x86_32/images/cxk_disk.img` |
| The same on a USB stick, the panic landing mid-transfer | add `--usb xhci` (or `ehci`, `ohci`) |
| Read the crash log a panic left on a disk image | `cxk rig crashlog <image>` |
| Host tests | `dotnet test devkit/CXEX.slnx -c Release` |
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
