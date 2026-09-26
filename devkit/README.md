# CX DevKit

**Toolchain and development environment for the CX ecosystem** — the CXK kernel, CXOS, the boot chain, and the X language.

This repository holds the **host side** of CX development: the X Native compiler, the `cxk` command-line toolchain, the CXEX format libraries, the signing tools, and CXEX Studio (the IDE). It is the counterpart to the [CXK](https://github.com/AuroraCrimsonRose/CXK) repository, which holds the kernel and the OS itself.

> Internal development tooling, tightly coupled to ongoing CX ecosystem work. Interfaces change without notice. See `LICENSE.md` (Catalyst Labs SDK License).

---

## What's actually in here

The two things most people come looking for:

- **`CXEX.Lang` — the X Native compiler.** A complete front-end and x86-32 back-end: lexer, parser, resolver, type checker, constant folder, and a GAS-syntax code generator. The entire CXK userland (shell, `std/`, GUI) is compiled by this.
- **`CXEX.CLI` — the `cxk` command.** Compiling, packaging, signing, disk imaging, inspection, and emulator launch.

### Project layout

| Project | Lines | Purpose |
|---|---:|---|
| `CXEX.Studio` | 2,417 | Avalonia IDE — project explorer, hex viewer, editors, emulator host |
| `CXEX.Lang` | 1,820 | **The X Native compiler** (see below) |
| `CXEX.CLI` | 1,447 | The `cxk` command-line toolchain |
| `CXEX.FileSystem` | 582 | CXFS, host side (format, read, write, browse) |
| `CXEX.Build` | 477 | CXEX packaging — ELF parsing, layout, writing |
| `CXEX.FileType` | 364 | Format/magic registry and identification |
| `CXEX.Disk` | 301 | Disk images, MBR / GPT / XBPT |
| `CXEX.Crypto` | 279 | RSA + SHA-256 keygen and signing |
| `CXEX.SDK` | 120 | SDK surface (early) |
| `CXEX.Core` | 95 | Shared primitives |

**Scaffolded but empty** — these have project files and no source yet: `CXEX.Font`, `CXEX.ICO`, `CXEX.Text`, `CXEX.Tools`, `CXEX.UI`. They are placeholders for planned work (see the design doc §5), not missing code. `CXEX.Tools` in particular is where the process-tool wrappers are *intended* to move so the CLI and Studio share one toolchain driver; today those wrappers still live in `CXEX.CLI/Wrappers`.

---

## The `cxk` CLI

```
cxk keygen      generate an RSA keypair (.xkpk / .xksk)
cxk compile     X source -> ELF
cxk build       ELF -> CXEX (.xcex / .xoex / .xkex)
cxk sign        attach a signature block to a CXEX artifact
cxk embed       CXEX -> C byte array header (for kernel-embedded images)
cxk image       build a bootable disk image
cxk raw-image   build a raw disk image
cxk run         launch an emulator against an image
cxk inspect     dump CXEX / disk / CXFS structure
cxk check       validation pass
```

### The compile pipeline, as it actually runs

```
foo.xfxn                          X Native source
  -> Lexer / Parser               AST
  -> Resolver / TypeChecker       typed AST  (ConstFold on the way)
  -> X86Emitter                   foo.s      GAS (AT&T) assembly text
  -> GccTool.Compile              foo.o      via the i686-elf cross toolchain
  -> GccTool.Link                 foo        ELF, against a generated linker script
  -> cxk build                    foo.xcex   CXEX-wrapped, signable
  -> cxk sign                     foo.xcex   signature block attached
```

The X front-end emits **assembly text**, not machine code, and leans on `i686-elf-gcc` to assemble and link. Dropping the external assembler by emitting CXEX sections directly is a possible later change — a back-end decision, not a language change.

Note two naming inconsistencies to be aware of when reading the code: X sources use `.xfxn`, but `CompileCommand`'s doc comment says `.x`, and the generated ABI prelude is named `abi.x`. The design doc's taxonomy (§3) says X Native source is `.XFXN`, so `abi.xfxn` would be the consistent name. Also, the taxonomy specifies an `.XCXN` compiled-object stage that **does not exist** — the cross toolchain's ELF plays that role today.

---

## Relationship to CXK, and the one coupling that matters

The kernel's syscall ABI is defined in **`CXK/abi/cxk_abi.h`**. The X compiler carries a copy of it as an X-language prelude, prepended to every compilation, in:

```
CXEX.Lang/Abi/AbiPrelude.cs
```

**That file emits a banner reading `GENERATED from cxk_abi.h — Do not edit by hand`, and nothing generates it.** It is a hand-maintained C# string literal, living in a different repository from the header it claims to track. There is no build step and no test connecting the two.

This has already cost a real bug: the kernel gained `SYS_MOUSE_READ` and `struct mouse_state`, the prelude did not, and the committed compiler could not compile the committed OS — `gui.xfxn` referenced two names that did not exist. The drift was exactly one syscall and one struct, and it was invisible until something failed to build.

Two ways to fix it, in increasing order of effort:

1. **Assert it.** A test that parses `cxk_abi.h` for `SYS_*` defines and ABI structs and fails if any is absent from the prelude. Roughly thirty lines, no build-system change, and it turns this class of bug into a red test. There is no test project in this repo yet, so this means adding one.
2. **Generate it.** A build step that emits `abi.x` from `cxk_abi.h`, making the banner true. Better, but it needs the two repositories visible to each other at build time, which is the awkward part.

Until one of those exists, **treat `AbiPrelude.cs` as requiring a manual update whenever `cxk_abi.h` changes.** The relevant CXK-side reference is `docs/CX_ABI.md`.

---

## Documentation

- **`docs/CX_DEVKIT_DESIGN.md`** — architecture, visual identity, artifact taxonomy, key authority and signing, Studio design, and the phased roadmap. The main design document for this repository.
- **`docs/index.md`** — entry point for the generated API reference (DocFX). `///` comments in source appear there on the next build.

The X *language* is specified on the kernel side, since the kernel owns the ABI it compiles against:

- `CXK/docs/CX_X_CORE_LANG.md` — the X core language spec (v0.2), including the route to self-hosting
- `CXK/docs/CX_ABI.md` — the syscall and capability contract
- `CXK/docs/CX_EXTENSION_SYSTEM.md` — the CXEX format and the signing scheme

---

## Building

Requirements: .NET (see the `.csproj` files for the target framework), and for producing CXK artifacts an `i686-elf` GCC cross toolchain plus NASM. QEMU or Bochs to run an image.

```
dotnet build CXEX.Studio.slnx
```

---

## Notes

- Not a general-purpose IDE; built specifically for CX ecosystem development
- i686 / 32-bit is the live target; other architectures are registry stubs
- Contains experimental and unstable tooling
- Long-term goal: X hosted on CXK, so the system can build its own software without a .NET host. See `CXK/docs/CX_X_CORE_LANG.md` §10.

---

© Aurora Tejeda / CATX SYSTEMS LLC
