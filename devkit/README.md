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
| `CXEX.CLI` | 2,198 | The `cxk` command-line toolchain |
| `CXEX.Lang` | 1,820 | **The X Native compiler** (see below) |
| `CXEX.Uefi` | 1,019 | UEFI Secure Boot — EFI variable stores, Authenticode PE signing |
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
cxk compile     X source -> ELF, or a linkable object (--object)
cxk build       ELF -> CXEX (.xcex / .xoex / .xkex)
cxk sign        attach a signature block to a CXEX artifact
cxk embed       CXEX -> C byte array header (for kernel-embedded images)
cxk image       build a bootable disk image
cxk raw-image   build a raw disk image
cxk run         launch an emulator against an image
cxk inspect     dump CXEX / disk / CXFS structure
cxk check       validation pass
cxk check-abi   verify the X ABI prelude still matches cxk_abi.h
cxk check-xdata validate X Data documents (service descriptors)

cxk secureboot keygen     generate a Secure Boot PK/KEK/db set
cxk secureboot varstore   enroll it into an OVMF EFI variable store
cxk secureboot sign       Authenticode-sign a PE so firmware will load it
cxk secureboot verify     would firmware holding this cert accept this image
cxk secureboot test       boot a stub under enforced Secure Boot, signed and unsigned
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

### What guards it now

```
cxk check-abi [path/to/cxk_abi.h]
```

Compares the header against the prelude and fails on real drift. It finds the header itself if you don't pass one (via `CXK_ROOT`, or a sibling CXK checkout). CXK's `tools/build.bat` runs it as a pre-flight beside the existing source check, so drift stops the build with a clear message rather than surfacing as "undefined name" errors inside `gui.xfxn`.

It reports three kinds of problem: a constant missing from a family the prelude mirrors, a constant whose **value** disagrees, and a struct whose **field order** disagrees — that last being the nastiest, since a reordered struct compiles fine on both sides and silently corrupts every call using it.

The prelude only mirrors part of the header (`SYS_*`, `E_*`, `POWER_*`, `FB_OP_*`, and the structs — not `CAP_*` or `NET_OP_*`), so the rule is *if a family is mirrored at all, it must be mirrored completely.* Unmirrored families are reported as notes, not failures. Add one `NET_OP_` constant and the rest become required.

The comparison lives in `CXEX.Lang/Abi/AbiSync.cs` rather than in the command, so a test project can call it — and **there is still no test project in this repository**, which is the remaining gap. `dotnet test` is the natural CI gate; today the check depends on running the build script.

**Treat `AbiPrelude.cs` as requiring a manual update whenever `cxk_abi.h` changes**, and run `cxk check-abi` after. The CXK-side reference is `docs/CX_ABI.md`.

Note the drift surface is wider than this one file: `os/std/net.xfxn` redeclares all seven `NET_OP_*` constants locally because the prelude doesn't carry them — a third copy of the ABI with no link back to the header.

### X Data: the second shared format

Service descriptors (`.xosv`) are X Data, specified in `CXK/docs/CX_X_DATA.md`. The supervisor reads them on the device with `os/std/xdata.xfxn`; the build checks them first with `CXEX.Lang/Data/XData.cs`, a line-for-line port of that reader:

```
cxk check-xdata <files...> [--keys exec,args,start,every,grants] [--porcelain]
```

A typo is then a build error naming `file:line:col`, not a line in a boot log. `--keys` refuses any top-level key outside the list, so a misspelt key cannot be silently ignored.

Two readers of one format are worth having only if they agree, so they are held to it: `tests/xdata/difftest.py` generates thousands of documents - valid, mutated, and nested past the depth limit - runs each through both readers (the X one compiled and run natively), and fails if any error code or byte offset differs. `SABOTAGE=1` skews one expectation, to show the test can fail.

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

### Tests

```
python3 tests/lang/run.py        # the X language: programs that must run, programs that must be refused, C <-> X interop, std/buf
python3 tests/xdata/difftest.py  # the DevKit's X Data reader against CXK's, document by document
```

Both run X natively on the host (a 32-bit `gcc` links the output), so no VM is involved. `tests/lang/refuse` holds programs the compiler must reject, each with the error it must give: every one of them used to compile and produce a wrong answer.

---

## Notes

- Not a general-purpose IDE; built specifically for CX ecosystem development
- i686 / 32-bit is the live target; other architectures are registry stubs
- Contains experimental and unstable tooling
- Long-term goal: X hosted on CXK, so the system can build its own software without a .NET host. See `CXK/docs/CX_X_CORE_LANG.md` §10.

---

© Aurora Tejeda / CATX SYSTEMS LLC
