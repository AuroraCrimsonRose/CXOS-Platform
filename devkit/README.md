# CX DevKit

**Toolchain and development environment for the CX ecosystem** — the CXK kernel, CXOS, the boot chain, and the X language.

`devkit/` holds the **host side** of CX development: the X Native compiler, the `cxk` command-line toolchain, the CXEX format libraries, and the signing tools. CXEX Studio, the Avalonia IDE, was retired on 2026-10-16; editor support is the VS Code extension in `editors/vscode`. The kernel and the OS it builds are the rest of this repository, [CXOS Platform](../README.md).

> Internal development tooling, tightly coupled to ongoing CX ecosystem work. Interfaces change without notice. See the repository's `LICENSE.md`.

---

## What's actually in here

The two things most people come looking for:

- **`CXEX.Lang` — the X Native compiler.** A complete front-end and x86-32 back-end: lexer, parser, resolver, type checker, constant folder, and a GAS-syntax code generator. The entire CXK userland (shell, `std/`, GUI) is compiled by this.
- **`CXEX.CLI` — the `cxk` command.** Compiling, packaging, signing, disk imaging, inspection, and emulator launch.

### Project layout

| Project | Lines | Purpose |
|---|---:|---|
| `CXEX.Lang` | 5,726 | **The X Native compiler** (see below) |
| `CXEX.CLI` | 3,149 | The `cxk` command-line toolchain |
| `CXEX.Uefi` | 1,019 | UEFI Secure Boot — EFI variable stores, Authenticode PE signing |
| `CXEX.FileSystem` | 582 | CXFS, host side (format, read, write, browse) |
| `CXEX.Build` | 551 | CXEX packaging — ELF parsing, layout, writing; console fonts (BDF to XFNT) |
| `CXEX.FileType` | 369 | Format/magic registry and identification |
| `CXEX.Disk` | 301 | Disk images, MBR / GPT / XBPT |
| `CXEX.Crypto` | 299 | RSA + SHA-256 keygen and signing |
| `CXEX.Core` | 102 | Shared primitives |
| `CXEX.Tests` | — | The xUnit test project (see [Tests](#tests)) |
| `CXEX.Tools` | — | External tool runners: clang, NASM, CMake, QEMU |


---

## The `cxk` CLI

```
cxk keygen      generate an RSA keypair (.xkpk / .xksk)
cxk compile     X source -> ELF, or a linkable object (--object)
cxk build       ELF -> CXEX (.xuex / .xoex / .xsex / .xkex / .xbex)
cxk sign        attach a signature block to a CXEX artifact
cxk embed       CXEX -> C byte array header (for kernel-embedded images)
cxk image       build a bootable disk image
cxk raw-image   build a raw disk image
cxk run         launch an emulator against an image
cxk inspect     dump CXEX / disk / CXFS structure
cxk check       validation pass
cxk check-abi   verify cxk_abi.h and the X prelude are what abi/kernel.xfil generates
cxk abi generate  write abi/cxk_abi.h from abi/kernel.xfil
cxk check-xdata validate X Data documents (service descriptors)

cxk tokens      print the tokens of X sources         \
cxk ast         print their syntax trees               |  what the compiler sees at each stage,
cxk sema        print each expression's type           |  compared against the compiler written
cxk asm         print the assembly compile generates   |  in X (CXK os/xc)
cxk prelude     print the ABI prelude                 /

cxk secureboot keygen     generate a Secure Boot PK/KEK/db set
cxk secureboot varstore   enroll it into an OVMF EFI variable store
cxk secureboot sign       Authenticode-sign a PE so firmware will load it
cxk secureboot verify     would firmware holding this cert accept this image
cxk secureboot test       boot a stub under enforced Secure Boot, signed and unsigned
```

**Planned** (decision D2 in [`docs/planning/HARDENING_PLAN.md`](../docs/planning/HARDENING_PLAN.md)): the
commands that replace CXK's `.bat` and `.sh` scripts, so building and running CXK is
the same on every host.

```
cxk os build [--dev]      configure and build CXK through CMake with Ninja, signing when a key is present (replaced tools/build.bat)
cxk run  --machine ...    q35 / AHCI / e1000 / packet capture options (replaces tools/run_qemu_ahci.bat)
cxk uefi build            build the UEFI stub (replaces boot/uefi/build.bat)
```

### `cxk secureboot`, and why it is in here

CXK's UEFI stub reads the firmware's `SecureBoot` variable. Testing that it does
so correctly means owning a platform: enrolling your own PK/KEK/db and signing
the stub with the db key, because firmware will not launch an unsigned binary and
Secure Boot switched off reports off no matter what the code does.

On Linux that is `openssl` + `virt-fw-vars` + `sbsign`. None of those exist on
Windows, and `virt-fw-vars` is Python. So the capability lives here instead, and
`CXEX.Uefi` depends on nothing outside the shared framework — no NuGet package,
no OpenSSL, no Windows SDK, no `signtool`.

Two things in there are worth knowing about, because both are invisible when
wrong:

- **Authenticode is not quite CMS.** `SignedCms` encodes `eContent` as an OCTET
  STRING as RFC 5652 requires; Authenticode puts the `SpcIndirectDataContent`
  SEQUENCE in raw, and the `messageDigest` attribute covers that SEQUENCE's
  *value octets*, not its full DER. Get either wrong and you produce a
  structurally valid signature that every verifier rejects. Written against
  `System.Formats.Asn1` for exactly that reason.
- **The EFI variable header's `TimeStamp` is at offset 0x10.** A reader and
  writer that agree on the wrong offset round-trip perfectly and firmware does
  not validate it, so the only symptom is another implementation reading a year
  of 65535 out of the 0xFF fill. The offsets are written out in
  `EfiVarStore`'s remarks.

### The compile pipeline, as it actually runs

```
foo.xfxn                          X Native source
  -> Lexer / Parser               AST
  -> Resolver / TypeChecker       typed AST  (ConstFold on the way)
  -> X86Emitter                   foo.s      GAS (AT&T) assembly text
  -> ClangTool.Compile            foo.o      assembled by clang --target=i686-elf
  -> ClangTool.Link               foo        ELF, linked by ld.lld against a generated linker script
  -> cxk build                    foo.xuex   CXEX-wrapped, signable
  -> cxk sign                     foo.xuex   signature block attached
```

The X front-end emits **assembly text**, not machine code, and leans on an external assembler and linker: **clang and ld.lld** (`docs/planning/HARDENING_PLAN.md`, D5). Clang's assembler produces byte-identical code for the compiler's output. Dropping the external assembler by emitting CXEX sections directly is a possible later change - a back-end decision, not a language change.

Note two naming inconsistencies to be aware of when reading the code: X sources use `.xfxn`, but `CompileCommand`'s doc comment says `.x`, and the generated ABI prelude is named `abi.x`. The design doc's taxonomy (§3) says X Native source is `.XFXN`, so `abi.xfxn` would be the consistent name. The taxonomy used to specify an `.XCXN` compiled-object stage; it was never built, and **ELF is now the decided linkable object format** (design doc §3, Q-E).

---

## Relationship to CXK, and the one coupling that matters

The kernel's syscall ABI is defined in **`abi/cxk_abi.h`**. The X compiler carries a copy of it as an X-language prelude, prepended to every compilation, in:

```
CXEX.Lang/Abi/AbiPrelude.cs
```

**That file emits a banner reading `GENERATED from cxk_abi.h — Do not edit by hand`, and nothing generates it.** It is a hand-maintained C# string literal, living in a different repository from the header it claims to track. Nothing generates it; a check (below) is what connects the two.

This has already cost a real bug: the kernel gained `SYS_MOUSE_READ` and `struct mouse_state`, the prelude did not, and the committed compiler could not compile the committed OS — `gui.xfxn` referenced two names that did not exist. The drift was exactly one syscall and one struct, and it was invisible until something failed to build.

### What guards it now

```
cxk check-abi [path/to/cxk_abi.h]
```

Since roadmap step 6a1 the header is itself generated, from `abi/kernel.xfil` (`cxk abi generate`). The check first fails if the header is not exactly what `kernel.xfil` beside it generates (a hand edit, or a forgotten `cxk abi generate`), or if this `cxk` was built from a different `kernel.xfil` (a stale `tools/cxk.exe`). Then it compares the header against the prelude and fails on real drift. It finds the header itself if you don't pass one, walking up from the working directory to the repository's `abi/` (or `CXK_ROOT`, if set). `cxk os build` runs it as a pre-flight beside the existing source check, so drift stops the build with a clear message rather than surfacing as "undefined name" errors inside `gui.xfxn`.

It reports three kinds of problem: a constant missing from a family the prelude mirrors, a constant whose **value** disagrees, and a struct whose **field order** disagrees — that last being the nastiest, since a reordered struct compiles fine on both sides and silently corrupts every call using it.

The prelude only mirrors part of the header (`SYS_*`, `E_*`, `POWER_*`, `FB_OP_*`, and the structs — not `CAP_*` or `NET_OP_*`), so the rule is *if a family is mirrored at all, it must be mirrored completely.* Unmirrored families are reported as notes, not failures. Add one `NET_OP_` constant and the rest become required.

The comparison lives in `CXEX.Lang/Abi/AbiSync.cs` rather than in the command, so a test project can call it. **That project is decided: `CXEX.Tests` (xUnit),** and a unit test running `AbiSync` against the real header is in its first batch, making `dotnet test` the gate. Until it lands, the check depends on running the CXK build.

**Treat `AbiPrelude.cs` as requiring a manual update whenever `cxk_abi.h` changes**, and run `cxk check-abi` after. The CXK-side reference is `docs/x86_32/kernel/CX_ABI.md`.

Note the drift surface is wider than this one file: `os/std/net.xfxn` redeclares all seven `NET_OP_*` constants locally because the prelude doesn't carry them — a third copy of the ABI with no link back to the header.

### X Data: the second shared format

Service descriptors (`.xosv`) are X Data, specified in `docs/x86_32/language/CX_X_DATA.md`. The supervisor reads them on the device with `os/std/xdata.xfxn`; the build checks them first with `CXEX.Lang/Data/XData.cs`, a line-for-line port of that reader:

```
cxk check-xdata <files...> [--keys exec,args,start,every,grants] [--porcelain]
```

A typo is then a build error naming `file:line:col`, not a line in a boot log. `--keys` refuses any top-level key outside the list, so a misspelt key cannot be silently ignored.

Two readers of one format are worth having only if they agree, so they are held to it: the differential test (`CXEX.Tests/Differential/XDataDiffTests`) generates documents - valid, mutated, and nested past the depth limit - runs each through both readers (the X one compiled and run natively), and fails if any error code or byte offset differs. `SABOTAGE=1` skews one expectation, to show the test can fail.

---

## Documentation

- **`docs/devkit/CX_DEVKIT_DESIGN.md`** — architecture, visual identity, artifact taxonomy, key authority and signing, the (retired) Studio design, and the phased roadmap. The main design document for this repository.
- **`docs/reviews/2026-10-01/DEVKIT_SECURITY_REVIEW.md`**, **`docs/reviews/2026-10-01/DEVKIT_ENGINEERING_REVIEW.md`** — the 2026-10-01 reviews of this repository, kept as written.
- **`docs/planning/HARDENING_PLAN.md`** — the response to both: decisions (xUnit, `cxk` commands in place of scripts, no tracked build output), the status of every finding checked against the code, and a phased checklist. Its kernel-side companion is `docs/planning/HARDENING_PLAN.md`.
- **`docs/devkit/index.md`** — entry point for the generated API reference (DocFX). `///` comments in source appear there on the next build.

The X *language* is specified on the kernel side, since the kernel owns the ABI it compiles against:

- `docs/x86_32/language/CX_X_CORE_LANG.md` — the X core language spec (v0.2), including the route to self-hosting
- `docs/x86_32/kernel/CX_ABI.md` — the syscall and capability contract
- `docs/x86_32/formats/CX_EXTENSION_SYSTEM.md` — the CXEX format and the signing scheme

---

## Building

Requirements: .NET (see the `.csproj` files for the target framework), and for producing CXK artifacts clang, ld.lld, NASM, CMake and Ninja. QEMU to run an image.

```
dotnet build devkit/CXEX.slnx
```

Prebuilt `cxk`, per platform, is on the repository's
[releases](https://github.com/AuroraCrimsonRose/CXOS-Platform/releases) page.

API reference (DocFX), from the repository root:

```
dotnet tool restore
dotnet docfx docs/devkit/docfx.json --serve
```

The tool manifest (`.config/dotnet-tools.json`) pins docfx. The output
(`docs/devkit/_site/`, `docs/devkit/api/`) is generated and ignored.

### Tests

**Decided: every suite moves to xUnit,** in a `CXEX.Tests` project in `CXEX.slnx`, run with:

```
dotnet test
```

**No Python interpreter is needed** — the harnesses were ported and deleted on
2026-10-08 (D1); only the corpus under `tests/` remains. Tests are grouped by
trait. `Unit` and `Adversarial` need only .NET. `Toolchain` needs clang and
ld.lld, on Linux or WSL. `Differential` compares against the OS sources in this
repository. A test whose requirement is missing reports **skipped**, with the
reason; it never silently passes.

`CXEX.Tests/Differential/` holds the ports:

| test | |
|---|---|
| `LexParseDiffTests` | the lexer and parser written in X (`os/xc`) against these, token for token and node for node |
| `SemaAsmDiffTests` | the type checker and code generator written in X against these, on whole programs |
| `SelfHostTests` | `xc` compiles itself twice: three identical assemblies, and the programs it builds must run |
| `LangRunTests` | the X language: programs that must run, programs that must be refused *for the stated reason*, C↔X interop, `std/buf` |
| `XDataDiffTests` | the DevKit's X Data reader against the OS's, document by document, on code **and** byte offset |

They run X natively on the host — `clang -m32 -nostdlib -static` links the
output, so no VM and no GCC are involved — which does mean they need an ELF
host, and they skip on Windows. `CXOS_HOSTCC` picks a different compiler for
that link.

`CXEX_TEST_MUTANTS` (default 50) and `CXEX_TEST_SEED` size and replay a run;
`KEEP=1` keeps the scratch directory. **`SABOTAGE=1` must turn the five
comparison tests red** — if it does not, they are comparing nothing.

`tests/lang/refuse` holds programs the compiler must reject, each pinned to the
error it must give: every one of them used to compile and produce a wrong
answer, and refusing for the wrong reason would pass a looser test.

---

## Notes

- Not a general-purpose IDE; built specifically for CX ecosystem development
- i686 / 32-bit is the live target; other architectures are registry stubs
- Contains experimental and unstable tooling
- Long-term goal: X hosted on CXK, so the system can build its own software without a .NET host. See `docs/x86_32/language/CX_X_CORE_LANG.md` §10.

---

© Aurora Tejeda / CATX Systems
