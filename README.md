# CXOS Platform

Everything that makes CXOS, in one repository: **CXK**, the kernel; the **X
userland**, including the X compiler written in X; the boot chain; and the **CX
DevKit**, the host toolchain (the `cxk` CLI, the C# X compiler, signing,
imaging) and CXEX Studio.

| Directory | What it holds |
|---|---|
| `abi/` | The syscall and boot ABI: the one source of truth both sides compile against |
| `boot/` | BIOS boot chain; `uefi/` stub |
| `kernel/` | CXK |
| `os/` | X userland: `apps/`, `std/`, `xc/` (the compiler in X), `executive/`, `config/`, `services/` |
| `tools/` | CMake build (`cmake/`), the platform public key, remaining scripts |
| `devkit/` | The C# toolchain and Studio, and their tests: see [`devkit/README.md`](devkit/README.md) |
| `editors/` | X language support for editors (VS Code) |
| `assets/` | Branding, UI icons, file-type icons |
| `docs/` | All documentation: start at [`docs/README.md`](docs/README.md) |

**Binaries are not in the repository.** `cxk`, CXEX Studio and the OS disk
image ship as [releases](https://github.com/AuroraCrimsonRose/CXOS-Platform/releases)
(`docs/planning/HARDENING_PLAN.md`, D7).

This repository joins the former CXK and CX_DEVKIT repositories, with both
histories kept (D6).

---

# CXK

**CATX Kernel**

The (C)apabilities and e(X)ecution (K)ernel for x86 systems.

CXK provides memory management, process execution, scheduling, privilege separation, storage access, networking, trust infrastructure, and executable loading services for the broader CX ecosystem.

The project is designed around modularity, verifiable execution, and reusable system capabilities. Rather than coupling operating system functionality directly into the kernel, CXK exposes foundational services that higher-level CX operating environments can build upon.

---

## Current Status

CXK is currently under active development.

The v5 architecture focuses on preserving proven subsystems from previous releases while modernizing the boot chain, executable loading infrastructure, and trust model. **The v5 port is complete** — see `docs/planning/history/V5_PORTING_MANIFEST.md`, retained as the historical plan.

Delivered:

- XKEX executable loading
- XBEX boot-stage execution
- CXFS integration
- Signed executable verification (RSA-2048 over SHA-256, enforced at the `cxex_exec` handoff)
- Ring 3 process execution, with per-process address spaces
- Capability-oriented kernel services, with attenuation at `spawn`
- X Native userland: shell, `std/` library, and a windowed GUI
- Filesystem access from ring 3, with per-process working directories
- Applications loaded and signature-verified from disk
- Multiple volumes, mounted under `/Drives/<drive>/<volume>`
- Services and scheduled tasks, run by a supervisor from `.xosv` descriptors
- A Key Vault deciding how far each signer is trusted
- Dynamic memory for ring 3 (`SYS_MEM_OP`), bounded by a per-process quota, and an allocator in X
- 64 and 128-bit integers in X, including divide and modulo
- Service descriptors in X Data, checked at build time as well as at boot
- Guarded kernel stacks: an overflow is a panic naming the thread, handled on a double-fault stack of its own
- Kernel configuration in X Data (`/System/Config/kernel.xkco`): stack size, thread limit and memory quota, read by X code linked into the kernel
- X: enums and sum types with exhaustive `switch`, struct and array initializers, character literals, compound assignment, values passed by value (C-compatible), and growable buffers in `std/buf.xfxn`
- The X compiler written in X (`os/xc`): `xc` produces exactly what the C# compiler does, compiles itself to a fixed point, and compiles itself on CXK

**Where it is going, and in what order, is in [`docs/planning/CX_ROADMAP.md`](docs/planning/CX_ROADMAP.md).** The current phase is self-hosting, X compiling X, which gates nearly everything else. Its stage 4 is done; stage 5 (an assembler and linker in X) follows hardening Phase 1.

**Security: under active hardening, not yet hardened.** The 2026-10-01 reviews ([`docs/reviews/2026-10-01/KERNEL_SECURITY_REVIEW.md`](docs/reviews/2026-10-01/KERNEL_SECURITY_REVIEW.md), [`docs/reviews/2026-10-01/KERNEL_ENGINEERING_REVIEW.md`](docs/reviews/2026-10-01/KERNEL_ENGINEERING_REVIEW.md)) found that the CXEX loader still trusts parts of a signed image's layout. Most seriously, it does not stop an image from mapping kernel addresses. Every finding has been checked against the code and planned in [`docs/planning/HARDENING_PLAN.md`](docs/planning/HARDENING_PLAN.md); closing the executable boundary is its Phase 1. Until then, do not treat signature verification as a complete defence against a malicious program.

---

## Core Concepts

### Capabilities

CXK is designed as a capabilities and execution kernel.

Core kernel services provide:

- Memory management
- Process scheduling
- User-mode execution
- Storage access
- Network access
- Power management
- Cryptographic verification
- Executable loading

These services form the foundation for future CX operating environments.

### Execution

Execution is a first-class concept within CXK.

The kernel currently supports:

- Ring 0 execution
- Ring 3 execution
- Cooperative multitasking
- Preemptive multitasking
- Kernel threads
- User processes
- Context switching

### Trust

CXK includes cryptographic infrastructure used to support signed executable deployment.

Current components include:

- SHA-256 implementation
- Public/private key tooling
- Signature verification infrastructure
- XKEX signing workflow

---

## Architecture

```text
BIOS
 └─ boot.asm
      ↓
   stage2.xbex
      ↓
   kernel.xkex
      ↓
   CXK
      ├─ Memory Management
      ├─ Process Scheduling
      ├─ Capability Services
      ├─ Storage
      ├─ Networking
      ├─ Power Management
      └─ Executable Loading
```

---

## Major Components

### CPU

- GDT
- IDT
- PIC
- FPU support
- Ring 3 transitions
- Context switching
- User-mode execution

### Memory Management

- Physical memory manager
- Paging
- Kernel heap
- Dynamic allocation

### Process Model

- Kernel threads
- User processes
- Cooperative scheduling
- Preemptive scheduling
- Privilege separation

### Storage

- ATA
- AHCI
- Disk abstraction layer

### Filesystem

- CXFS filesystem

### Networking

- Intel e1000 support
- ARP
- IPv4 foundation
- ICMP Echo (Ping)

### Power

- ACPI support
- Shutdown
- Reboot
- CPU idle support

### Video

- VGA text mode
- Framebuffer support
- VBE initialization

---

## CATX Format Ecosystem

CXK uses a structured CATX format naming convention.

Format names follow:

```text
X + Domain + Type
```

Examples:

| Format | Description |
|----------|----------|
| XKEX | Kernel Executable |
| XKPK | Kernel Public Key |
| XKSK | Kernel Secret Key |
| XBSG | Boot Signature |
| XPMF | Package Manifest |

This naming system allows tooling and developers to identify broad file purpose directly from the extension.

See:

- docs/formats/CX_EXTENSION_NAMING.md
- docs/formats/CX_EXTENSION_SYSTEM.md

for additional details.

---

## v5 Goals

The primary objective of v5 is to modernize executable loading and deployment while preserving proven subsystems.

### New

- XBEX stage-2 boot execution
- XKEX loading infrastructure
- CXEX runtime loader
- Signed executable workflow

### Ported From Previous Versions

- Memory management
- Scheduler
- Ring 3 execution
- ATA and AHCI
- Networking
- Power management
- CXFS

### Future

- UEFI boot support
- Expanded networking stack (UDP, DNS, TCP)
- Additional CX formats
- Enhanced CXFS features
- User-space executable ecosystem

### Long-term goal: building CXK from CXK

The objective is a **self-sufficient system** — one that compiles its own software, on itself, with no external host. The X toolchain currently runs on .NET, so CXK can execute X programs but cannot yet produce them. Closing that gap is the long-term direction, and it drives the language roadmap: see `docs/language/CX_X_CORE_LANG.md` §0 and §10 for what self-hosting requires and the staged route to it.

---

## Building

> **Moving to `cxk` commands.** The `.bat` scripts below are being replaced by
> cross-platform `cxk` commands (decision D2 in
> [`docs/planning/HARDENING_PLAN.md`](docs/planning/HARDENING_PLAN.md)). Each script is removed
> when its replacement lands:
>
> | Today | Replacement |
> |---|---|
> | `tools\build.bat [dev]` | `cxk os build [--dev]` (planned) |
> | `tools\run_qemu_ahci.bat` | `cxk run` with machine options: q35, AHCI, e1000, packet capture (planned) |
> | `tools\run_bochs.bat` | `cxk run -e bochs` (works now) |
> | `boot\uefi\build.bat` | `cxk uefi build` (planned) |
> | `boot\uefi\secureboot.bat` | `cxk secureboot keygen` / `varstore` / `sign` / `test` (work now) |
>
> **The toolchain is moving to LLVM on every host** (decision D5): clang
> replaces the i686-elf GCC cross toolchain, ld.lld links, and Ninja
> replaces NMake. When that lands, the MSVC Developer Command Prompt and the
> GCC cross toolchain stop being requirements; NASM stays. Tests run in the DevKit with `dotnet test`; no Python is needed.

Requirements:

- **MSVC Developer Command Prompt** — `build.bat` configures CMake with `-G "NMake Makefiles"`, so `nmake` must be on PATH. Run the build from a Developer Command Prompt (or after `vcvarsall.bat`), not a plain shell.
- **i686-elf GCC cross toolchain** — compiles and links everything in `kernel/`. Paths come from `tools/cmake/cxk_toolchain.cmake`.
- NASM — the boot chain and the kernel's assembly
- CMake
- QEMU
- Bochs (optional)
- **`tools/cxk.exe`** — the DevKit's CLI. CMake drives it for packaging, signing,
  imaging and X compilation. **It is not committed.** Take it from the latest
  [release](https://github.com/AuroraCrimsonRose/CXOS-Platform/releases)
  (`cxk-win-x64.exe`, saved as `tools/cxk.exe`; `cxk-linux-x64` and
  `cxk-osx-arm64` for other hosts), or publish it from `devkit/` as a single
  file and copy it to `tools/cxk.exe`:

  ```bat
  dotnet publish devkit\CXEX.CLI -c Release -r win-x64 -p:SelfContained=true -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
  ```

  Republish whenever the DevKit changes something the build uses. The
  checks the build runs through it (`check-abi`, `check-xdata`) warn and skip on an older
  one; the kernel itself needs `cxk compile --object` (it links X code), and CMake stops
  at configure time with this instruction if the `cxk` it finds is too old.

Build:

```bat
tools\build.bat
```

The build **signs automatically when a signing key is present** (`tools\kernel.xksk`,
created once with `tools\cxk.exe keygen tools\kernel`). Without one it builds
unsigned — and a release kernel refuses to launch unsigned programs, so an unsigned
release build boots to the kernel and stops.

Build a **development kernel**, which runs unsigned programs, so testing needs no
signing key at all:

```bat
tools\build.bat dev
```

A development kernel says so at every boot and logs each unsigned program it
admits. It admits only programs with *no* signature: a tampered image, a forged
signature, or a file that is not a program is refused exactly as in a release
kernel. It is a build option (`-DDEV_UNSIGNED=ON`), never a runtime setting, so a
release kernel contains no code that can run an unsigned image — and CMake refuses
to build one that is both signed and development. **Never ship a development
kernel.**

Run:

```bat
tools\cxk.exe run dist\CXK_x86_32\images\cxk_disk.img
```

AHCI + e1000 test, until `cxk run` has the machine options:

```bat
tools\run_qemu_ahci.bat
```

---

## Documentation

Everything is under [`docs/`](docs/README.md), by topic:

| Folder | Covers |
|---|---|
| `docs/kernel/` | The syscall and capability ABI; the process model |
| `docs/system/` | CXFS; the OS filesystem layout |
| `docs/formats/` | CXEX, the extension system and naming, file structure |
| `docs/language/` | The X language and X Data |
| `docs/devkit/` | The DevKit design document and its API reference (DocFX) |
| `docs/planning/` | The roadmap; the hardening plan; history |
| `docs/reviews/` | External reviews, kept as written |

> **Note for anyone changing `abi/cxk_abi.h`:** the X compiler carries a hand-maintained copy
> of this ABI as an X prelude (`devkit/CXEX.Lang/Abi/AbiPrelude.cs`). Nothing generates it
> yet, so **a syscall or ABI struct added here must be added there in the same change** or the
> toolchain will not compile the userland. `tools\build.bat` runs `cxk check-abi` before
> building, which reports drift; a `CXEX.Tests` unit test is planned to check it on every
> test run. See `docs/devkit/CX_DEVKIT_DESIGN.md` §5.2.

---

## License

See LICENSE.md.

---

© Aurora Tejeda / CATX SYSTEMS LLC
