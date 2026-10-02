# CXOS Platform — Review Response & Hardening Plan

**Responds to:** the four reviews of 2026-10-01 in [`docs/reviews/2026-10-01/`](../reviews/2026-10-01/):
kernel security, kernel engineering, DevKit security, DevKit engineering.
**Status:** decisions recorded 2026-10-01. The repository merge (D6) is done; Phase 0 is in progress.

This document turns the reviews into decisions and one ordered checklist. The
reviews stay as written; this is where their findings are tracked to done.
Each finding was checked against the code before being planned. Where a
finding is confirmed, this says so. Where it changed since the review, or the
review was missing something, this says that too.

It replaces the two plans the CXK and CX_DEVKIT repositories carried before
they were merged.

**Until Phase 1 is done, CXK is not hardened.** Signed-executable verification
is real, but the loader that runs after it still trusts the image's layout.
Nothing should be described as hardened or zero-day resistant before then.

---

## 1. Decisions

### D1. Tests move to xUnit, in `CXEX.Tests`; Python is retired

Host-side testing runs on the DevKit's own stack: `dotnet test`, with no Python
interpreter required. A new **`CXEX.Tests`** xUnit project joins the DevKit
solution. That closes the "there is no test project" gap the DevKit design doc
(§5.2, §14 Phase 0.5) and the DevKit engineering review (§4) both name.

```
devkit/CXEX.Tests/
  Unit/         direct tests of each library: Core, Crypto, Disk, FileSystem, Build, Lang, Uefi, Abi
  Adversarial/  malformed inputs: DevKit security review §14 (ELF, CXEX, crypto, files/tooling)
  Lang/         replaces devkit/tests/lang/run.py   - run/, refuse/, interop/, std/
  XData/        replaces devkit/tests/xdata/difftest.py
  XC/           replaces devkit/tests/xc/*.py       - lexdiff, parsediff, semadiff, asmdiff, selfhost
```

- **Categories, by trait.** `Unit` and `Adversarial` need nothing but .NET.
  `Toolchain` needs clang and ld.lld (D5) and a Linux host or WSL, because it
  runs compiled X natively and X's host platform makes Linux system calls.
  `Differential` compares against the OS sources in this repository. A test
  whose requirement is missing reports **skipped, with the reason**. It never
  silently passes.
- **Mutant counts and seeds** come from `CXEX_TEST_MUTANTS` and
  `CXEX_TEST_SEED`. The defaults are small enough for every run; a nightly or
  pre-release run sets thousands.
- **`SABOTAGE=1` becomes a test.** Each comparison harness has a unit test
  proving it reports a planted difference, so it can never quietly compare
  nothing.
- **Test data stays in `devkit/tests/`.** The `.xfxn` programs are language
  data; only the scripts go.
- **Each Python script is deleted in the change that ports it.**

**Kernel tests stay in the kernel:** the `ktest.c` self-tests run at boot,
under real paging and real ring 3, which a host test cannot reproduce. They gain
adversarial cases (Phase 1) and lifecycle cases (Phase 2).

### D2. System-specific scripts are replaced by `cxk` commands

One cross-platform tool, `cxk`, builds, runs and tests the platform. The
`.bat`, `.ps1`, `.cmd` and `.sh` files go: replaced by a `cxk` command, or
deleted where already dead. Each is removed in the change that lands its
replacement.

| Script | Today | Replacement |
|---|---|---|
| `tools/build.bat [dev]` | Checked the toolchain, ran the `cxk check-abi` pre-flight, signed if `tools/kernel.xksk` existed, configured CMake for NMake, built. Windows and MSVC prompt only. | **`cxk os build [--dev]`** — **done; the script is deleted.** Same steps, including both pre-flights, configuring CMake with Ninja and clang (D5), and it now probes the toolchain up front so a missing tool is named rather than surfacing as a CMake error. |
| `tools/run_qemu.bat` | **Dead.** It passes `-M pc --fs cxk_filesystem.img` to `cxk run`, which has neither option, and the build no longer produces a separate filesystem image. | **`cxk run dist/CXK_x86_32/images/cxk_disk.img`**, which works today. **Deleted** in the merge. |
| `tools/run_qemu_ahci.bat` | Raw QEMU line: q35, AHCI, e1000, packet capture, 4 GB, PC speaker. | **`cxk run` machine options** (`--machine q35`, `--net e1000`, `--pcap FILE`, `--mem`, `--speaker`). |
| `tools/run_bochs.bat` | Already only calls `cxk run -e bochs`. | **`cxk run -e bochs`**. Delete the script. |
| `os/executive/build.sh` | **Dead.** It calls `tools/CXEX_Compiler/mkcxes.py` and `signcxex.py`, which no longer exist; CMake builds the executive. | None needed. **Deleted** in the merge. |
| `boot/uefi/build.bat` | Builds the UEFI stub with MSVC `cl` and `link`. | **`cxk uefi build`**, using clang and lld-link (D5). |
| `boot/uefi/secureboot.bat` | Generates a Secure Boot key set, enrolls it into an OVMF variable store, signs the stub, and boots it signed and unsigned. Every step already calls `cxk`. | **`cxk secureboot keygen`, `varstore`, `sign` and `test`**, which already exist. Delete the script once `boot/uefi/README.md` gives the sequence. |
| `devkit/build-docs.cmd` | Does not work as committed: no `.config/dotnet-tools.json`, so `dotnet tool restore` restores nothing. | `dotnet tool restore` + `dotnet docfx docs/devkit/docfx.json --serve`, with a tool manifest pinning docfx. |

`tools/cmake/` stays: CMake remains the build engine, and `cxk os build` drives it.

### D3. Generated output is not tracked

Nothing a build writes is committed. Done in the merge:

- `os/executive/app_image.h` (208 KB, written by the CMake build) is untracked
  and ignored.
- The histories were cleaned when they were merged, since the new repository
  had no clones to break. Removed from every commit:
  - **CXK:** 8 copies of `tools/cxk.exe` (about 38 MB each), two release
    zips, and the retired `hello.xcex` and `shell.xcex`.
  - **DevKit:** the published CLI binary (75 MB), `bin/`, `obj/`, `.vs/` and
    the generated DocFX site (`docs/_site`, 28 MB; `docs/api`).
- One `.gitignore` covers the whole repository.

Signing and Secure Boot key material (`*.xksk`, `*.xusk`, `sbkeys/`, `*.pfx`,
`*.pem`, `*.cer`, `*_VARS.fd`, `*.signed.efi`) stays ignored. Neither history
ever contained any; this was checked before the merge.

### D4. Order of work

Phase 0 (tooling) first, so the fixes after it are tested the new way. Then
Phase 1 (the executable boundary). Then 2 and 3. Stage 5 of the roadmap (an
assembler and linker in X) starts after Phase 1, and runs alongside Phases 2
and 3.

### D5. One toolchain on every host: LLVM, with Ninja

Building CXK and its UEFI stub uses the same tools on Windows, Linux and macOS.

- **`clang --target=i686-elf`** compiles the kernel's C. It replaces the
  i686-elf GCC cross toolchain. Checked 2026-10-01: all 71 kernel C files
  compile with the build's existing flags, with no errors and no warnings.
- **clang's integrated assembler** assembles the X compiler's output. Checked:
  for `xc`'s own 70,000-line assembly, its `.text` is **byte-identical** to
  GNU `as`'s.
- **`ld.lld`** links the kernel, executive and programs. **Done and booted.**
  The kernel's `linker.ld` needed no change: the higher-half `AT()` split comes
  out byte-for-byte as intended (`.text` VMA `0xC0100000` / LMA `0x00100000`,
  `.pagetables` outside `__bss_start..__bss_end`). Two things did have to change,
  both because GNU ld had been papering over them:
  - **SSE.** `clang --target=i686-elf` defaults to an SSE2-capable CPU and used
    `movsd`/`%xmm0` for ordinary 64-bit integer moves. The kernel never sets
    `CR4.OSFXSR`, so the first one raised `#UD` inside `pmm_init`. The kernel
    flags now carry `-mno-sse -mno-sse2 -mno-mmx -mno-implicit-float`; x87 stays
    on, because `lib/math/kmath.c` genuinely uses `double`.
  - **Page alignment.** `os/executive/executive.ld` and `os/apps/hello.ld` had no
    `ALIGN` between sections and relied on GNU ld page-aligning LOAD segments by
    itself. ld.lld packs them as written, so `.rodata` shared page `0x00400000`
    with `.text`; the CXEX loader maps section by section, so mapping `.rodata`
    replaced the entry point's page and the executive faulted on its first
    instruction. Both scripts now align each section explicitly, as
    `kernel/linker.ld` always did.
- **`clang` + `lld-link`** builds the UEFI stub. `boot/uefi/README.md` already
  documents this route.
- **Ninja** is the CMake generator everywhere. **Done:** the MSVC Developer
  Command Prompt and NMake are no longer requirements, on any host.
- **NASM stays** for the boot sector and stage 2, which are NASM syntax and
  build the same on every host.

Stage 5 of the roadmap later replaces the assembler and linker with X's own;
this decision covers the time until then.

On the DevKit side, `cxk compile` assembles and links with clang and ld.lld:
`GccTool` is gone, replaced by `ClangTool` (`CXEX.CLI/Wrappers/ClangTool.cs`),
which moves to `CXEX.Tools` with the other wrappers in Phase 2.

### D6. One repository: CXOS Platform

CXK and CX_DEVKIT are merged into this repository, with both histories kept.
They already behaved as one project: every change was a matched pair of PRs,
the ABI prelude was copied by hand across them, and every differential test
needed both checked out. Layout:

```
abi/        the syscall and boot ABI: the one source of truth
boot/       BIOS boot chain; uefi/ stub
kernel/     the kernel (linker.ld included)
os/         X userland: apps, std, xc (the compiler in X), executive, config, services
tools/      cmake/ build, the platform public key, remaining scripts
assets/     branding, icons, file-type icons
devkit/     the C# toolchain: CXEX.* projects, tests
editors/    editor support for X (VS Code)
docs/       kernel/, system/, formats/, language/, devkit/, planning/, reviews/
```

### D7. Binaries ship as releases, never as commits

`cxk`, CXEX Studio and the OS image are built by CI and attached to a GitHub
Release, per platform. Nothing they produce is committed.

| Artifact | Release asset |
|---|---|
| `cxk` CLI | single-file, self-contained: `win-x64`, `linux-x64`, `osx-arm64` |
| CXEX Studio | self-contained app: `win-x64`, `linux-x64`, `osx-arm64` |
| OS disk image | `cxk_disk.img` today; an ISO once ISO distribution lands |

Building CXOS from source takes `cxk` from the latest release, or from
`dotnet publish devkit/CXEX.CLI`, into `tools/`.

The image job needs a toolchain CI can install, so it lands with D5. A
**signed** release image needs the platform private key at build time. That is
an open decision (§5): sign locally and upload, or keep the key as a CI secret.

---

## 2. Kernel review findings

### 2.1 Kernel security review

| § | Finding | Checked against code | Phase |
|---|---|---|---|
| 1 | **Critical:** the CXEX loader can map kernel addresses | **Confirmed.** `k_map_page` (`lib/format/cxex_loadk.c:30`) passes the image's virtual address straight to `paging_map`, with no check against `KERNEL_VBASE`. | 1 |
| 2 | **High:** signature-offset checks can wrap | **Confirmed.** The bounds checks at `cxex.c:87`, `:104` and `:107` add untrusted values before comparing them. | 1 |
| 3 | **High:** more wrap and exhaustion paths in loading | **Confirmed.** `va_end` and the `fb_hi` sum in `cxex_load.c` can wrap, and image pages are not counted against the process quota. | 1 |
| 4 | `user_ptr_ok()` does not check writability | **Confirmed** (`cpu/usermode.c:79`). It checks presence and the user bit only, so a kernel write through a read-only user page succeeds. | 1 |
| 5 | W^X differs between `vm_map()` and CXEX loading | **Confirmed.** `prot_from_section` grants write and execute together when a section asks for both. | 1 |
| 6 | IPC reply buffer lifetime / TOCTOU | **Plausible, mostly future.** Only one ring-3 process runs at a time today, so the window cannot be reached. It becomes real with concurrent user threads. Fix it by copying replies through the kernel before that. | 2 |
| 7 | IPC endpoint exhaustion | **Confirmed.** `cpu/ipc.c` has a fixed table of 32 endpoints (`MAX_ENDPOINTS`) and no way to free one. | 2 |
| 8 | Make VM invariants hard to violate | Adopted: separate `paging_map_user` and `paging_map_kernel`, so the user path cannot express a kernel address. | 1 |
| 9 | Existing controls | Kept as they are. | — |
| 10 | Test plan | Becomes adversarial `ktest.c` cases (Phase 1). The DevKit adds matching host-side cases (`CXEX.Tests/Adversarial`). | 1–2 |
| — | **Added:** the signature must cover every byte the loader reads | The review does not name this. The kernel hashes `[0, signature_offset)` but does not require each section's file bytes to lie inside that range, so data placed after the signature is not authenticated. The loader rejects such an image; the DevKit writer stops producing one. | 1 |
| 12.1 | Enforce one cryptographic profile | **Confirmed gap, the only outstanding item in §12.** `rsa_parse_xkpk` (`lib/crypto/rsa.c:23`) reads `version` and `key_bits` and discards both — the comment says "(not strictly needed)" — and never looks at the exponent or the reserved field. So the kernel will parse a CXPK of any declared version, any key size its modulus buffer fits, and **any exponent, including 1 and even values**, which the DevKit refuses. The DevKit is stricter but not strict enough: `ParseCxpkHeader` reads `Version` and never checks it, and pins neither `key_bits == 2048` nor `exponent == 65537`. `cxk keygen --bits` will generate a key of any size, including one this kernel cannot verify. | 2 |
| 12.2 | Keep the trust root separate from the embedded key | **Already correct.** `cxex_verify` compares the carried `.xkpk` **byte for byte** against the compiled-in root (`cxex_verify.c:97`) and returns `CXEX_VERIFY_WRONG_KEY` otherwise; `keyvault.c` then resolves platform vs publisher vs untrusted the same way. The property the review cares about holds: a cryptographically valid signature from a key the machine was never told to believe is refused. | — |
| 12.3 | Protect private keys as trust-root material | **Already correct.** `.gitignore` covers `*.xksk`, `*.xusk`, `*.pem`, `*.pfx`, `*.cer`, `sbkeys/` and now `tools/*.xkpk` except the tracked platform public half; `SECURITY.md` states that a committed private key is a vulnerability even after the branch is deleted, and treats it as a trust-root compromise requiring rotation. | — |
| 12.4 | Preserve exact-byte key identity | **Already correct, and load-bearing.** The fingerprint hashes the serialized `.xkpk` bytes, and trust is decided by byte comparison rather than by re-deriving the key — so no normalization step exists to disagree with the fingerprint. The requirement this creates, that CXPK have exactly one canonical serialization, is now written down in `docs/formats/CX_KEY_FORMAT.md`. | — |
| 12.5 | Keep signing and verification independent | **Already correct.** The signer uses .NET's RSA; CXK verifies with its own `bignum`/`rsa`/`sha256`. Neither depends on the other, and CXK is the authority at the execution boundary. | — |
| 12.6 | Make the signed-range invariant explicit | **Done 2026-10-01**, as the "Added" row above. `check_section` returns `CXEX_LOAD_UNSIGNED` for a section whose file bytes reach past `signature_offset`, and the writer cannot produce such an image. | 1 |
| 12.7 | Crypto-policy regression tests | **Partly done.** `CXEX.Tests/Adversarial/CryptoTests.cs` has 16 cases: malformed, truncated, oversized and zero-length keys, `key_bits` disagreeing with `modulus_len`, degenerate exponents, signing refused for a malformed image, a malformed key, a mismatched pair and an already-signed image, and signatures failing over modified payload, metadata or fingerprint. **Missing, because the checks themselves are missing:** wrong key size rejected, wrong algorithm identifier rejected, unsupported CXPK version rejected, non-zero reserved rejected. These land with 12.1. | 2 |

### 2.2 Kernel engineering review

| § | Recommendation | Status | Phase |
|---|---|---|---|
| 1 | The ATA backend is narrower than the disk API | **Partly fixed.** `disk_read`/`disk_write` now split requests into 128-sector pieces (`DISK_XFER_MAX`), so the 8-bit count can no longer truncate. The LBA is still cast to 32 bits for ATA (`disk.c:131`, `:148`). It must be rejected with `DISK_ERR_PARAMS` when it does not fit. | 1 |
| 2 | Bounded-string contracts | To do. | 2 |
| 3 | Scheduler modes and lifecycle | To document in `PROCESS_MODEL.md`. | 3 |
| 4 | Per-process kernel stacks | **Largely in place.** Every ring-3 process has its own `esp0` stack, and `update_tss_esp0` runs on each switch (`PROCESS_MODEL.md` §8). Kernel-only threads use `kstack_top == 0` by design. To do: write the invariant down as a contract. | 3 |
| 5 | Overflow-safe lexer lookahead | **Confirmed.** `lx_peek` in `os/xc/lex.xfxn` uses `lx.pos + n >= lx.len`. Change it to a subtraction-based check and audit the other range checks in `os/xc`. The differential tests must stay identical. | 1 |
| 6 | Which lexer is authoritative | **Done** (`CX_X_CORE_LANG.md` §10): the C# compiler is the oracle while X matches it; an intentional X change updates the grammar and the test corpus; the X compiler becomes normative once it is authoritative. | 3 |
| 7 | Sleep upper bound; timer wraparound | **Confirmed gap.** `test_clock_sleep` checks the lower bound only. | 2 |
| 8 | Block-device contract | To write before VFS work starts (roadmap). | 2 |
| 9 | Error handling and failure semantics | Ignored return values and cleanup after partial initialisation get one audit pass. | 2 |
| 10 | Resource ownership and teardown | With IPC reclaim (security §7). | 2 |
| 11 | Initialisation ordering | To document. | 3 |
| 12 | ABI stability | `CX_ABI.md` v2 exists, and the DevKit's `cxk check-abi` already verifies its prelude against the header. To do: mark and version each ABI structure. | 3 |
| 13 | Localise x86 assumptions | Ongoing. | 3 |
| 14 | Fatal vs recoverable failures | To define. | 3 |
| 15 | Retire transitional code | The first sweep is D2's dead scripts and D3's tracked `app_image.h`. | 0 |
| 16 | Lifecycle tests | New `ktest.c` cases. | 2 |

---

## 3. DevKit review findings

### 3.1 DevKit security review

| § | Finding | Checked against code | Phase |
|---|---|---|---|
| 1 | ELF parsing must validate | **Confirmed.** `ElfParser.cs` (71 lines) checks magic, class and endianness only. Program-header bounds, `p_offset + p_filesz`, `p_filesz <= p_memsz`, counts and overlaps are unchecked. Being C#, a bad ELF throws rather than corrupting memory, but it can still reach layout code malformed. | 1 |
| 2 | CXEX layout arithmetic must be checked | Confirmed in shape: layout uses unchecked `int`/`uint` sums. | 1 |
| 3 | **Critical:** signing must cover everything security-relevant | **Confirmed, and sharper than the review states.** CXK hashes `[0, signature_offset)`, but neither side requires every section's file bytes to lie inside that range. Section data placed after the signature is unauthenticated. Fix both sides: the writer places all section data before the signature, and the kernel rejects any image where it is not. | 1 |
| 4 | Separate trust authorities | Largely in place: `PLATFORM`/`PUBLISHER` tiers and the Key Vault (design doc §4, Q-B settled). To do: write down the authority transitions and add tests that a publisher key cannot produce a platform-tier artifact. | 2 |
| 5 | Secure Boot trust distinct from CXEX trust | Holds today: separate key sets (`cxk secureboot keygen` against `cxk keygen`), separate formats. To do: state it as an invariant in the design doc and add a test. | 2 |
| 6 | Explicit crypto policy | Kernel side is fixed: RSA-2048 with SHA-256, others rejected (`cxex_verify.c`). DevKit side to write down: key size, hash, encoding, fingerprint format, and the rejection paths. | 2 |
| 7 | Private-key handling | `*.xksk`, `*.pfx`, `*.pem`, `sbkeys/` are gitignored. To audit: command-line exposure, logging, temporary files, default locations, file permissions. | 2 |
| 8 | Filesystem paths as a boundary | To do: atomic output (write to a temp file, then replace), input/output collision checks, cleanup on failure. | 1 (atomic output), 2 (rest) |
| 9 | External tool execution | Process runner lives in `CXEX.CLI/Wrappers`. To do: explicit executable, environment, timeout and version rules, when it moves to `CXEX.Tools` (engineering §6). | 2 |
| 10 | Sign only after validation | Pipeline to formalise: parse → validate → policy → canonical serialise → sign. | 1 |
| 11 | DevKit and CXK validate independently | Adopted as an invariant. The kernel-side loader checks are CXK Phase 1. | 1 |
| 12 | Deterministic artifacts | To verify and test: identical inputs produce identical CXEX bytes. | 3 |
| 13 | Supply chain | Long-term: pinned toolchain versions, recorded tool identity, reproducible DevKit builds. | 3 |
| 14 | Security regression tests | Become `CXEX.Tests/Adversarial` (D1). | 0 (harness), 1 (cases) |

### 3.2 DevKit engineering review

| § | Recommendation | Status | Phase |
|---|---|---|---|
| 1 | Validated ELF model | Same work as security §1 | 1 |
| 2 | CXEX layout as an intermediate model | Same work as security §2, §3 | 1 |
| 3 | parse → validate → transform → emit | Adopted as the pipeline rule | 1 |
| 4 | A .NET test project | **Decided: `CXEX.Tests` (D1)** | 0 |
| 5 | Artifact taxonomy (`.XCXN`) | **Decided:** ELF is the linkable object; `.XCXN` is dropped from the taxonomy (design doc §3, Q-E). | done |
| 6 | Shared tool execution in `CXEX.Tools` | Already planned (design doc Phase 2, Q-D) | 2 |
| 7 | Canonical ABI source | `cxk check-abi` mitigates now; a generator is planned (design doc §5.2 item 2). | 3 |
| 8 | Library → CLI/Studio layering | Adopted as a rule. Studio must not reference `CXEX.CLI`. | 2 |
| 9 | Explicit toolchain versions | With security §9, §13 | 2–3 |
| 10 | Structured errors across library boundaries | With the `CXEX.Tools` move | 2 |
| 11 | Deterministic generation | Same as security §12 | 3 |
| 12 | File/image lifecycle semantics | Document per API as each is touched | 3 |
| 13 | Central version/format compatibility | With the CXEX layout model | 2 |
| 14 | Classify transitional code | Ongoing. The first sweep is the stale `.slnx` entries, the tracked DocFX output (D3) and the five empty scaffold projects (README). | 3 |

---

## 4. Checklist

### Phase 0 — tooling and repository (D1–D3, D5–D7)

- [x] Merge CXK and CX_DEVKIT into CXOS Platform, histories kept, build output stripped (D6, D3).
- [x] Delete the dead scripts: `tools/run_qemu.bat`, `os/executive/build.sh` (D2).
- [x] Untrack `os/executive/app_image.h`; one `.gitignore` (D3).
- [ ] `CXEX.Tests` (xUnit), with the traits and environment variables of D1; port `lang/run.py`, `xdata/difftest.py`, `xc/*.py`, deleting each. **Scaffolded 2026-10-01** (xunit.v3, in the solution, `dotnet test` green): `Categories` holds the four traits; `TestEnv` reads `CXEX_TEST_MUTANTS`/`CXEX_TEST_SEED`/`SABOTAGE` and locates the repo, `cxk` and the tools; `Requires` turns a missing prerequisite into a **skip carrying its reason** — verified by running the suite outside a checkout, where it skips rather than passing. The nine Python scripts are still to port, each deleted as it lands.
- [x] Unit test: `AbiSync` against the real header (DevKit design doc §5.2, Q-F). Three tests in `CXEX.Tests/Unit/AbiSyncTests.cs`: the prelude matches `abi/cxk_abi.h`; the comparison is not vacuous (it parsed constants and structs from both sides); and a planted value mismatch is reported, so `AbiSync.Compare` is proven able to fail. `dotnet test` is now the gate, without an OS build.
- [x] Toolchain to LLVM (D5): clang, ld.lld and Ninja in the CMake build; prove the ld.lld link, then boot and pass `ktest.c` before removing the GCC path. `cxk compile` on clang/ld.lld. Done 2026-10-01: ld.lld link verified against `linker.ld`, then a dev image booted in QEMU to `self-tests: all 23 passed`, shell and supervisor up, and no CPU exception taken anywhere in the boot. The GCC path is removed — `CROSS_PREFIX`/`CROSS_SUFFIX` and `GccTool` are gone.
- [ ] `cxk os build [--dev]`, the `cxk run` machine options, `cxk uefi build`; delete each script they replace (D2). **`cxk os build [--dev]` is done** and `tools/build.bat` is deleted; it adds a toolchain pre-flight that names a missing tool instead of failing inside CMake. The `cxk run` machine options and `cxk uefi build` are still outstanding, so `tools/run_qemu_ahci.bat` and `boot/uefi/build.bat` stay for now.
- [ ] Docs tooling: `.config/dotnet-tools.json` pinning docfx; delete `devkit/build-docs.cmd`; drop the stale `.slnx` entries.
- [x] Release workflow (D7): `cxk` and CXEX Studio per platform; the OS image once D5 lands. The image job is in `release.yml` as of 2026-10-01: a clean ubuntu runner installs clang, lld, nasm, ninja-build and cmake with apt, publishes `cxk`, and runs `cxk os build` — which generates `trusted_key.c` itself as of 2026-10-02, from the public half of whichever key signs, so the workflow no longer has a hand-written embed step that could name a different key than the build signs with. **Correction, 2026-10-02: this workflow has never run.** Every run since it was added is a `startup_failure` at 0 seconds — on branch pushes as well as tags, which is the tell that GitHub never got as far as reading the triggers — and there are no releases at all, so no tag has ever published anything. The cause is not the file: js-yaml parses it, `actionlint` 1.7.7 reports nothing, the committed blob is valid UTF-8 with LF endings, the job ids are well formed and `needs` names jobs that exist. It is billing — the repository is private, so runs consume paid minutes, and the run page says *"The job was not started because recent account payments have failed or your spending limit needs to be increased."* So the claim this item used to make, that the image job is "the only continuous check that D5's one toolchain on every host is true", was **never true of anything**: every build to date has been on Windows and nothing has verified clang/ld.lld/Ninja on Linux. Releases are built locally in the meantime (`docs/planning/VERSIONING_AND_RELEASE.md` §5), with artifacts named identically so enabling Actions later changes nothing a user sees. The image is **signed** now, by a key generated for the build and destroyed after it — `cxos-<version>-<name>-selfsigned.img`; an image signed by the **platform** key still waits on §5.
- [ ] File-type integration: a `cxk` command that registers the CX file types with their icons, replacing the old Winkit `.reg` files. **Blocked on art:** `assets/icons/` was deleted 2026-10-02 — the bundled third-party set went with it, and the CX file-type icons are to be redrawn rather than rebuilt from what was there. The command can be written against an empty icon set; it cannot ship without one.

### Phase 1 — the executable boundary (critical / high)

**Complete, 2026-10-02.** Every item below is ticked, on both sides of the
boundary. What the phase actually bought, stated plainly: an untrusted image can
no longer name a kernel address, cannot be both writable and executable, cannot
carry bytes the signature does not cover, cannot wrap an offset or a size past a
check, cannot be placed at all until the whole of it has passed, and cannot cost
more memory than its process was given. None of that is taken on trust — the
refusals are driven at every boot by twelve adversarial cases through mock ops,
each one a single mutation of a known-good image that is itself a case, and each
refusal also asserts that nothing was mapped. Phase 1 did not only close the
findings: the stage-2 truncation, the CXSG layout disagreement, the missing CXPK
magic check, `vm_map` stranding pages on a refusal and the spawn failure paths
leaking an entire address space were all found while closing them, and none of
the five is in either review.

**Kernel**

- [x] Two-phase loader: **validate the whole image, then allocate and map.** Nothing is mapped until every section has been checked. Done 2026-10-01. `cxex_load` runs a validation pass over every section before it allocates a single frame; the checks live in one `check_section` so both passes cannot drift apart. Previously each section was validated as it was mapped, so a bad section half way through left the earlier ones already mapped into a live address space.
- [x] Every section's address range lies wholly in user space: overflow-safe, and below `KERNEL_VBASE` (security §1). Done 2026-10-01. `virt_addr + mem_size` is summed in 64 bits and compared against a `va_limit` the caller supplies — `cxex_loadk.c` sets it to `KERNEL_VBASE`. It is passed in rather than hard-coded because `cxex_load.c` is deliberately free of kernel headers so it stays host-testable; a zero limit is refused outright, so a caller that forgets it fails loudly instead of silently losing the check.
- [x] `paging_map_user` / `paging_map_kernel`, with the loader able to call only the first (security §8). Done 2026-10-02. `paging_map` is gone; the shared body is a private `paging_map_raw` and the two entry points are the whole public surface. They are deliberately asymmetric: the **kernel** one takes any virtual address, because drivers map MMIO wherever the BAR put it, but refuses `PAGE_USER` — so a kernel mapping can never be reachable from ring 3; the **user** one adds `PAGE_USER` itself but refuses any address at or above `KERNEL_VBASE`, so a ring-3 mapping can never name kernel memory. Neither call can express the dangerous combination, so no caller has to remember not to. All 34 kernel-side call sites were renamed and the 7 user-side ones converted to check the new return value — `vm_map` now unwinds through `undo()` on refusal rather than stranding the pages it had already mapped. A `PAGE_USER` passed to the kernel entry point is reported, not silently stripped: stripping it would hand the caller a mapping that quietly does not work.
- [x] Overflow-safe offset and size checks throughout `cxex.c` and `cxex_load.c` (security §2, §3). Done 2026-10-01. `cxex_get_sig` compared in `size_t`, which is **32 bits here**, so a `signature_offset` near the top of the address space wrapped to a small number, passed the bounds test and then read a gigabyte past the image. That and the two sums derived from it are now 64-bit, as are every `file_offset + file_size` and `virt_addr + mem_size` in the loader.
- [x] Every section's file bytes lie within `[0, signature_offset)` (added finding). Done 2026-10-01, in the loader's validation pass. The DevKit refuses to write such an image and the kernel refuses to load one, which is the independent validation the reviews ask for (security §11).
- [x] Image pages are charged to the process memory quota (security §3). Done 2026-10-02. The loader's `CXEX_LOAD_MAX_PAGES` cap (64 MB) was only ever the outer bound; the quota a process is actually given is 16 MB, and nothing placed by the loader counted against it — `vm_info` reported only what `SYS_MEM_OP` had asked for, so a process could hold an image, a 256 KB stack and an argument page that the quota never saw. `vm_charge` / `vm_uncharge` now account for user pages that are not mmap regions, using the same three checks `vm_map` makes in the same order (a rounded length that wrapped, a sum that wrapped, the ceiling) so two accounts kept by different arithmetic cannot disagree on the inputs chosen to make them disagree. The loader asks through a new `charge_pages` op, called **once with the whole bill between the two passes** — after validation has worked out what the image needs and before pass 2 has allocated anything — so a refused image costs no frames at all, where charging as the pages arrived would leave the refusal holding everything it got before the ceiling. `spawn.c` charges the stack and the argument page the same way, before their frames are allocated. **It also fixes a leak the charging made obvious:** every failure path in `proc_trampoline` simply called `thread_exit()`, leaving the half-built process's user frames, page tables and page directory behind — so an image the loader *refuses* could be spawned in a loop to drain the PMM, the refusal doing the draining. The exit path's teardown is now a shared `proc_teardown` that every failure path takes. Verified by boot, in both directions: 26/26 with the charging in place and no CPU exception anywhere, and with the charge call and the ceiling check sabotaged, exactly the two tests that cover them fail.
- [x] `user_ptr_readable` / `user_ptr_writable`, and every syscall uses the right one (security §4). Done 2026-10-01. `user_ptr_ok` checked presence and the USER bit only, so a read-only user page passed it and the kernel wrote through anyway — ring 0 ignores the read-only bit unless CR0.WP is set. A process could hand a syscall a pointer into its own text and have the kernel scribble on it. The single function is **removed**, not aliased, so all 38 call sites had to say which access they meant and a new one cannot default to the weaker check: 21 readable, 17 writable. `paging_is_user_writable` checks PAGE_WRITE on **both** the PDE and the PTE, because the CPU ANDs privilege bits across the two levels.
- [x] W^X: refuse a section that is both writable and executable (security §5). Done 2026-10-01 in the loader's validation pass. This matters more on x86 without PAE than it reads: there is no per-page execute bit, so a writable page **is** executable whether or not anyone intended it, and refusing the combination at load is the only place the rule can be enforced at all.
- [x] ATA: reject an LBA beyond 32 bits with `DISK_ERR_PARAMS` (engineering §1). Done 2026-10-02, and the limit is tighter than the review says: `ata.c` is **LBA28**, not 32-bit — the top nibble goes in the drive-select register (`(lba >> 24) & 0x0F`), so the ceiling is `0x0FFFFFFF`, 128 GB. `(uint32_t)lba` therefore truncated twice, at 4 GB and again at 128 GB, each time landing the transfer on a real but **wrong** sector. On a write that is the worst kind of failure: success reported, damage done elsewhere. Both `disk_read_once` and `disk_write_once` now refuse out-of-range addresses, and the sector count is bounded for the same reason even though `DISK_XFER_MAX` already keeps callers inside it. A boot test proves the rejection — **and announces when it cannot run**: on q35 the disk arrives through AHCI (LBA48), so the test is vacuous there and says so rather than returning a quiet pass. Verified for real on `-machine pc`, legacy IDE, where it exercises the ATA path.
- [x] `os/xc` lexer: subtraction-based range checks (engineering §5). Done 2026-10-02. `lx_peek` tested `lx.pos + n >= lx.len` on two `u32`s; the sum wraps, and a wrapped sum compares small, so a position near the top of the range passed the bounds test and indexed past the source. It now checks `lx.pos >= lx.len` first and then `n >= lx.len - lx.pos`, so the subtraction is only evaluated once it is known not to underflow and the arithmetic cannot leave the range at all. `lx_eof` and `lx_cur` already had the safe form. The C# `Lexer.Peek` had the same shape on an `int` and was changed to match — the two are held to identical behaviour by the differential tests, and that should include the edges. **Audited the rest of `os/xc`:** the other `i + 1 <` comparisons are in `dumputil.xfxn` (bounded by `xc_argc()`) and `emit.xfxn` (bounded by list lengths), none reachable with a value that could wrap. **Verification is partial and worth knowing:** the C# change is proven behaviour-preserving — token output over the whole X corpus, 93570 lines, is byte-identical before and after — and the X change is equivalent by construction, compiles, and boots. What could NOT be run here is `lexdiff`, the differential test that compares the two lexers token for token: it is one of the Python suites needing Linux and `gcc -m32`, which is exactly what D1 exists to retire.
- [x] Adversarial `ktest.c` cases: a kernel-range address, wrapped offsets, an oversized image, data past the signature, a W+X section, a write through a read-only user pointer. Done 2026-10-02, in `kernel/ktest_loader.c` (kept out of `ktest.c`, which is already 960 lines, and it brings its own image builder). Eleven cases for the loader plus one for pointer writability, run at every boot. Two things make them worth more than their line count: they drive `cxex_load` through **mock ops**, so a case that should be refused cannot damage anything if it is not — the test reports the failure instead of corrupting the address space — and each refusal also asserts that **nothing was mapped**, because a loader that refuses after mapping half the image has still placed the attacker's pages. Every case is one mutation of a known-good image, and the known-good image is itself a case, so the suite cannot pass by refusing everything.

**DevKit**

- [x] `ElfParser`: full structural validation (security §1), returning a validated model. Done 2026-10-01. `Parse` returns `ElfImage`, whose existence is the guarantee: header well formed (ET_EXEC, EM_386, ELFCLASS32/LSB), `e_phentsize` exactly 32, the program-header table wholly inside the file with `e_phoff + e_phnum * e_phentsize` computed in 64 bits, per segment `p_offset + p_filesz` inside the file and `p_filesz <= p_memsz` and `p_vaddr + p_memsz` inside the 32-bit space, `p_align` a power of two that `p_vaddr` and `p_offset` agree modulo, no two PT_LOADs overlapping, a cap on segment count and on aggregate memory, and an entry point that lands in an executable segment. Every rejection is an `InvalidDataException` naming the field and value.
- [x] CXEX layout model with checked arithmetic; reject overlaps, wrap-around and absurd counts or sizes (security §2). Done 2026-10-01 on the **read** side: a `CXEXExecutable` that loaded without throwing is the validated model. `Load` checks the format version, caps `section_count`, requires `section_offset` clear of the header and `section_offset + count * 28` inside the file in 64-bit arithmetic, and per section `file_size <= mem_size`, `file_offset + file_size` inside the file and clear of the section table, `virt_addr + mem_size` inside the 32-bit space, no two sections overlapping in memory, and `FLAG_SIGNED` agreeing with `signature_offset`. `GetSectionData` re-checks its own range, since `Sections` is public and mutable. The **write** side is done too: `CXEXLayoutEngine` and `CXEXWriter` both sum sizes and offsets in 64 bits, refuse a section count past the 16-bit field, and refuse a payload that disagrees with its declared `file_size` — the old `uint totalSize` could wrap and allocate a buffer smaller than the data about to be written into it.
- [x] **Added, not in either review: the signer and the reader disagreed about the CXSG block.** `CXEXParser` treated the header as 42 bytes and read `sig_len` from offset 40 - which is `pubkey_len` - then placed the signature at `offset + 42`, where `sig_len` itself lives. The authority is the kernel's `struct cxex_sig`: a 44-byte header, then `pubkey_len` bytes of .xkpk, then `sig_len` bytes of signature. Proved on a real signed image, which reported a 272-byte signature (the .xkpk's length) where RSA-2048 produces exactly 256 - and `cxex_verify.c` refuses anything but `key.modulus_len`. So `CXVerifier` was verifying the length field plus the first 270 bytes of the public key. It went unnoticed because every image this repository builds is unsigned by default, so the signed path was never read back. Fixed, and locked by a round-trip test through the real `CXKeyGenerator`/`CXSigner`.
- [x] Writer: all section data before the signature, so the signed range covers every byte the loader reads (security §3). Done 2026-10-01. `CXEXWriter` requires every section's bytes to lie in `[header+table, end of image)`, and the signature is appended past that end - so data before it is data the signature covers, by construction. `CXEXLayoutEngine` lays sections out contiguously from the end of the table and refuses an image whose data passes 4 GB, where a 32-bit `file_offset` would wrap back into the header.
- [x] Writer: refuse sections that are both writable and executable (matches CXK Phase 1 W^X). Done 2026-10-01, in **both** `CXEXLayoutEngine` (from the ELF flags) and `CXEXWriter` (from the CX flags). Deliberately twice: `WriteExecutable` takes a layout from any caller and does not assume the engine ran first (security §11).
- [x] Sign only validated, canonically serialised images; atomic output (security §8, §10). Done 2026-10-01. `CXSigner` is now parse → validate → sign: it loads the image through `CXEXExecutable.Load` and refuses to sign one that is malformed, refuses to sign an image that already carries a signature (which would hash the first signature as payload and leave two CXSG blocks), validates the `.xkpk` as a real CXPK before taking a fingerprint over it, and checks that the private key is the other half of the public key travelling with the image — signing with one and shipping the other produces an artifact the kernel reports as `BAD_SIGNATURE`, which reads as tampering rather than as the wrong file on a command line. Both `CXEXWriter` and `CXSigner` write to a temporary file and move it over, so an interrupted run cannot leave a patched header pointing at a signature block that was never written.
- [x] `CXEX.Tests/Adversarial`: the ELF, CXEX and crypto cases of security §14. **The ELF cases are done** (2026-10-01): 18 tests in `Adversarial/ElfParserTests.cs`, one per §14 bullet, built from `Elf32Builder` so each case is a single mutation of a known-good image. They need no toolchain, no checkout and no built OS, so they run on every host. A rejection must be `InvalidDataException`/`NotSupportedException` — an `IndexOutOfRange` counts as a failure, since that is the parser falling off the end of the file rather than deciding anything. **The CXEX cases are done too** (2026-10-01): 13 tests in `Adversarial/CxexLoadTests.cs` against `CXEXExecutable.Load`, including the Critical one — a section whose bytes lie past `signature_offset`, which the signature does not cover. Both suites are paired with a test that loads **real build output** from `dist/`, because an adversarial suite proves nothing about the images that matter: a validator rejecting everything passes it completely. That test skips, with its reason, when the OS has not been built. **The crypto cases are done** (2026-10-01): 16 tests in `Adversarial/CryptoTests.cs` — malformed, truncated, oversized and zero-length public keys, `key_bits` disagreeing with `modulus_len`, degenerate exponents, signing refused for a malformed image, a malformed key, a mismatched key pair and an already-signed image, and signatures that stop verifying over a modified payload, modified metadata or a corrupted fingerprint. This closes §14.

#### Found and fixed 2026-10-02: the kernel was 1.6 KB from being silently truncated at boot

Adding roughly 4 KB of **unrelated, never-called** code to the kernel makes
`test_kconfig` — "kernel config (X Data, linked X)" — fail. Nothing else changes
and no other test is affected.

Narrowed by bisection rather than reasoning, because the first two explanations
were both wrong:

- it is **not** the content of the new code. A probe of 200 trivial arithmetic
  functions (`return x * i + (i ^ 0x5a)`), referenced by nothing, reproduces it
  exactly;
- it is **not** merely adding a translation unit. A 16-byte one is fine;
- it is **not** `.bss`. Moving 5 KB of buffers from `.bss` to the heap left
  `__kernel_end` identical to the baseline and the failure unchanged;
- it is **not** position in `KERNEL_C_SRCS`. First and last behave the same.

What was left was the size of `.text` itself, and the cause was in the boot
chain, not the kernel:

```
boot/stage2.asm:  KERNEL_SECTORS equ 512   ; 256 KB of headroom for the kernel
                                           ; (Kernel is ~50KB now: 5x room.)
```

Stage 2's read loop reads **exactly** that many sectors. The comment was years
stale: the kernel had reached **260533 bytes — 99.4% of the 256 KB budget**, with
1611 bytes to spare. Anything past it was simply left on disk, with no check and
no error. The machine booted and ran; only the tail of `.text` was missing — and
`kx_xdata.o` is linked last, so the X Data reader was the first thing off the
end, which is why the one visible symptom was a config-parser test.

The `KSNT` patch does not save it: that feeds `cxexload`'s *copy*, while the
*read* loop uses the constant.

Fixed twice over:

- `KERNEL_SECTORS` is 1024 (512 KB). The load buffer at `0x10000` then ends at
  `0x90000`, leaving a 60 KB gap before the protected-mode stack at `0x9F000`.
- **`cxk` now refuses to build an image whose kernel exceeds the budget**, naming
  both constants and what to do. Raising the ceiling only buys time; the real fix
  is that overflowing it can no longer be silent. Verified by setting the budget
  to 100 and watching the build fail with
  `the kernel is 263229 bytes (515 sectors), beyond the 100 sectors stage 2 reads`.

Confirmed by booting a kernel of **515 sectors** — past the old cap — to
`self-tests: all 25 passed`, with no CPU exception anywhere in the boot.

### Phase 2

**Kernel — IPC, resources, contracts**

- [ ] **One cryptographic profile, enforced on both sides (security §12.1, §12.7).** CXK is RSA-2048 / SHA-256 / PKCS#1 v1.5 and nothing else, but nothing says so in code. `rsa_parse_xkpk` must check the CXPK version, require `key_bits == 2048` and `modulus_len == 256`, require the exponent to be the platform's (65537), and require the reserved field to be zero — it currently discards version and key_bits as "not strictly needed" and never looks at the exponent, so it will parse a key with exponent 1, which makes every signature trivially forgeable for any caller that does not also do the byte comparison. The DevKit's `ParseCxpkHeader` reads `Version` and never checks it; `cxk keygen` must refuse `--bits` other than 2048 rather than generating a key this kernel cannot verify. The CXSG algorithm identifier names this exact profile, and a future profile gets a **new identifier** rather than being accepted under the old one. Then the four missing cases in §12.7 become testable and go in: wrong key size, wrong algorithm id, unsupported CXPK version, non-zero reserved. The format these enforce is specified in `docs/formats/CX_KEY_FORMAT.md`.
- [ ] IPC endpoints are reference-counted and freed when their last handle closes (security §7, engineering §10).
- [ ] IPC replies copied through the kernel before concurrent user threads exist (security §6).
- [ ] Sleep upper-bound and timer-wraparound tests (engineering §7).
- [ ] Block-device contract and bounded-string contracts (engineering §2, §8).
- [ ] Error-path audit (engineering §9). Lifecycle tests (engineering §16).

**DevKit — trust, tooling and policy**

- [ ] Write down the crypto policy and authority transitions; test that a publisher key cannot sign platform-tier artifacts (security §4–§6).
- [ ] Private-key handling audit (security §7).
- [ ] Move the process runner to `CXEX.Tools` with explicit execution rules and structured errors (security §9; engineering §6, §10).
- [ ] Enforce library → front-end layering (engineering §8); central version/format compatibility (engineering §13).

### Phase 3

**Kernel — documentation and architecture**

- [ ] Scheduler modes, the `esp0` invariant and initialisation order, in `PROCESS_MODEL.md` (engineering §3, §4, §11).
- [x] The authoritative-lexer rule in `CX_X_CORE_LANG.md` §10 (engineering §6). Written with this plan.
- [ ] Versioned ABI structures; fatal-vs-recoverable rules; x86 assumptions kept local (engineering §12–§14).

**DevKit — maturity**

- [ ] Deterministic CXEX output, tested (security §12, engineering §11).
- [ ] Toolchain identity recorded; pinned versions (security §13, engineering §9).
- [ ] ABI generator (engineering §7).
- [ ] Transitional-code sweep (engineering §14).

---

## 5. Decided since, and open

- **`.XCXN`:** not built. ELF is the linkable object format (DevKit design doc §3, Q-E).
- **Toolchain:** clang, ld.lld and Ninja everywhere; clang + lld-link for UEFI (D5).
- **Repository:** one repository, CXOS Platform (D6).
- **Releases:** binaries ship as release assets (D7).
- **Open — release signing:** sign release images locally and upload them, or hold the platform key as a CI secret? A CI secret is convenient, but anyone who can change a workflow can then sign a kernel. **The technical blocker is gone as of 2026-10-02:** a local test key pair (`cxk os build --key test`) has now carried a fully signed image through a boot — signed kernel, signed executive, signed programs, no development kernel underneath — and the kernel admitted every one of them on its real signature. Until then the signed path had never been read back end to end, which is exactly why the CXSG layout disagreement in §3 survived as long as it did. What remains is the **policy** question above, not whether it works.
