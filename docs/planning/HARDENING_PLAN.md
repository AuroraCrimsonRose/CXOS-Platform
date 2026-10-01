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
| `tools/build.bat [dev]` | Checks the toolchain, runs the `cxk check-abi` pre-flight, signs if `tools/kernel.xksk` exists, configures CMake for NMake, builds. Windows and MSVC prompt only. | **`cxk os build [--dev]`**: the same steps, including the ABI pre-flight, configuring CMake with Ninja and clang (D5). |
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
- **`ld.lld`** links the kernel, executive and programs from the existing
  linker scripts. This has not been tried yet; it is the first thing the
  migration checks.
- **`clang` + `lld-link`** builds the UEFI stub. `boot/uefi/README.md` already
  documents this route.
- **Ninja** is the CMake generator everywhere. The MSVC Developer Command
  Prompt and NMake stop being requirements.
- **NASM stays** for the boot sector and stage 2, which are NASM syntax and
  build the same on every host.

Stage 5 of the roadmap later replaces the assembler and linker with X's own;
this decision covers the time until then.

On the DevKit side, `cxk compile` assembles and links with clang and ld.lld,
and `GccTool` becomes an LLVM wrapper when the wrappers move to `CXEX.Tools`.

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
- [ ] `CXEX.Tests` (xUnit), with the traits and environment variables of D1; port `lang/run.py`, `xdata/difftest.py`, `xc/*.py`, deleting each.
- [ ] Unit test: `AbiSync` against the real header (DevKit design doc §5.2, Q-F).
- [ ] Toolchain to LLVM (D5): clang, ld.lld and Ninja in the CMake build; prove the ld.lld link, then boot and pass `ktest.c` before removing the GCC path. `cxk compile` on clang/ld.lld.
- [ ] `cxk os build [--dev]`, the `cxk run` machine options, `cxk uefi build`; delete each script they replace (D2).
- [ ] Docs tooling: `.config/dotnet-tools.json` pinning docfx; delete `devkit/build-docs.cmd`; drop the stale `.slnx` entries.
- [ ] Release workflow (D7): `cxk` and CXEX Studio per platform; the OS image once D5 lands.
- [ ] File-type integration: a `cxk` command that registers the CX file types with their icons (`assets/icons/filetypes`), replacing the old Winkit `.reg` files.

### Phase 1 — the executable boundary (critical / high)

**Kernel**

- [ ] Two-phase loader: **validate the whole image, then allocate and map.** Nothing is mapped until every section has been checked.
- [ ] Every section's address range lies wholly in user space: overflow-safe, and below `KERNEL_VBASE` (security §1).
- [ ] `paging_map_user` / `paging_map_kernel`, with the loader able to call only the first (security §8).
- [ ] Overflow-safe offset and size checks throughout `cxex.c` and `cxex_load.c` (security §2, §3).
- [ ] Every section's file bytes lie within `[0, signature_offset)` (added finding).
- [ ] Image pages are charged to the process memory quota (security §3).
- [ ] `user_ptr_readable` / `user_ptr_writable`, and every syscall uses the right one (security §4).
- [ ] W^X: refuse a section that is both writable and executable (security §5).
- [ ] ATA: reject an LBA beyond 32 bits with `DISK_ERR_PARAMS` (engineering §1).
- [ ] `os/xc` lexer: subtraction-based range checks (engineering §5).
- [ ] Adversarial `ktest.c` cases: a kernel-range address, wrapped offsets, an oversized image, data past the signature, a W+X section, a write through a read-only user pointer.

**DevKit**

- [ ] `ElfParser`: full structural validation (security §1), returning a validated model.
- [ ] CXEX layout model with checked arithmetic; reject overlaps, wrap-around and absurd counts or sizes (security §2).
- [ ] Writer: all section data before the signature, so the signed range covers every byte the loader reads (security §3).
- [ ] Writer: refuse sections that are both writable and executable (matches CXK Phase 1 W^X).
- [ ] Sign only validated, canonically serialised images; atomic output (security §8, §10).
- [ ] `CXEX.Tests/Adversarial`: the ELF, CXEX and crypto cases of security §14.

### Phase 2

**Kernel — IPC, resources, contracts**

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
- **Open — release signing:** sign release images locally and upload them, or hold the platform key as a CI secret? A CI secret is convenient, but anyone who can change a workflow can then sign a kernel.
