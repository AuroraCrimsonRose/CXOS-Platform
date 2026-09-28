# CX DevKit — Architecture, Vision & Roadmap

**Owner:** Aurora Tejeda · **Company:** CATX Systems LLC · **Products:** CX, CXK, CXOS
**Doc status:** **v0.3** — the v0.2 Q&A decisions remain **locked** (marked **[LOCKED]**); genuinely-open items are in §13. This revision reconciles the document with what has actually shipped and moves it out of `CXEX.Studio/` (it describes the whole DevKit, not just Studio).

> **Implementation status at a glance.** What exists today: the X Native compiler
> (`CXEX.Lang` — lexer, parser, resolver, type checker, x86-32 emitter), the ten-command
> `cxk` CLI, CXEX packaging, host-side CXFS, disk/partition models, RSA+SHA-256 keygen and
> signing, and a working Studio shell. §11's bugs 1 and 2 are **fixed**. Five projects named
> in §5 are **scaffolded but empty** (`CXEX.Font`, `CXEX.ICO`, `CXEX.Text`, `CXEX.Tools`,
> `CXEX.UI`). Phases 1 onward in §14 are open.
>
> **Where this document is aspirational, it says so.** Sections describing Studio windows,
> font formats, the authority-tier key header, the installer target and the SDK are design,
> not description — none of that is built yet.

Logic lives in libraries; CLI and Studio are thin front-ends over the same code. The look is IDE-like but distinctly CX. Build in small verifiable checkpoints.

---

## 1. Vision

The CX DevKit is the single ergonomic platform for building the CX ecosystem — the CXK kernel, CXOS, bootloaders, and X-language executables — across multiple target architectures, replacing the Python/batch/PowerShell toolchain.

Two faces of one core:
- **CX DevKit (Studio)** — full IDE for first-party kernel/OS/boot/app development, packaging, signing, imaging, debugging.
- **CX SDK** — a future, slimmer, **separate** Avalonia app for third-party developers building X apps for CXOS. Shares the `CXEX.UI` control/theme library so the two read as one product. **[LOCKED]**

**Licensing context:** apps built with the DevKit (and anything built to run on CXOS) are MIT to their authors; the kernel/OS themselves are proprietary CATX. This shapes the key-authority model (§4) and the SDK (§12).

---

## 2. Visual Identity **[LOCKED]**

IDE-like layout, distinctly-CX chrome. Concrete direction from your references:

- **Navigability like VS Code** — discoverable menus and toolbars. **Do not** hide actions behind a command-palette-only model (the "weird Shift+P" problem). Everything reachable by eye.
- **Flat & simple like Zed**, but with a touch more substance/contrast — not as ultra-smooth/minimal as Zed.
- **High-contrast, bright syntax highlighting** — readability first.
- **Spyder-style icon button bar** — a clean, obvious icon toolbar where "I need to press X" is instant.

**Brand palette**

| Hex | Role | Notes |
|---|---|---|
| `#111827` | Primary surface (deep navy-black) | Darkest layer; main background |
| `#5BC0F8` | Primary accent (CX cyan) | Interaction: active states, selection, focus, primary buttons |
| `#F48FB1` | Semantic highlight (CX pink) | "Look here": magic bytes, entry/exit points, attention |
| `#EAF4FF` | Foreground (near-white blue) | Primary text/icons on dark surfaces |

Derived surface ramp from `#111827` for layered panels/borders (e.g. `#0B0F19 / #111827 / #1B2433 / #2A3650`). You'll annotate as you see it live.

---

## 3. Artifact & File-Type Taxonomy **[LOCKED]**

Pattern: **`XF**` = Format (source)**, **`XC**` = Compiled**, executables are CXEX-wrapped `X_EX`.

**X language source & compiled**

| Ext | Name | Meaning |
|---|---|---|
| `.XFXN` | cX Format, X Native | X Native (systems core) source |
| `.XFXR` | cX Format, X Runtime | X Runtime dialect source |
| `.XFXH` | cX Format, X Hybrid | X Hybrid dialect source |
| `.XCXN` | cX Compiled, X Native | Compiled X Native object |

**There is no `.XCXR` or `.XCXH`, and there should not be.** The dialect is a
property of the SOURCE, not of what comes out. XR and XH desugar to X core and
share one backend (`CX_X_CORE_LANG.md` §0), so a compiled XH program is
byte-for-byte the same kind of artifact as a compiled XN one — and under the
domain rule an executable is named for **whose** it is, not for what produced
it. An XH program written for a user is a `.xuex`, exactly like an XN one.

Carrying the dialect into the binary would also undo the thing the split buys:
a loader that had to know which front end emitted an image is a loader that can
be wrong about it.

**Compile/link chain:** `*.XFXN` / `*.XFXR` / `*.XFXH` (source) → **`*.XCXN`**
(compiled linkable object — one form, whichever dialect it came from) →
**packaged executable** (`.xkex` / `.xbex` / `.xoex` / `.xsex` / `.xuex`
depending on **whose** it is). `XCXN` is the intermediate; the `X_EX` family is
the final CXEX-headered, signable artifact.

> **Stale as of the domain restructure.** This table said `XCEX` above. `.xcex`
> is retired: it named an executable for how it was BUILT, and every executable
> is compiled, so the letter distinguished nothing. See
> `CXK/docs/CX_EXTENSION_SYSTEM.md` §2.

> **As-built deviation — `.XCXN` does not exist.** The word appears nowhere in the codebase.
> The pipeline that actually runs is:
>
> ```
> foo.xfxn -> foo.s (GAS text) -> foo.o -> foo (ELF) -> foo.xuex
>             X86Emitter          i686-elf-gcc          cxk build
> ```
>
> The cross toolchain's **ELF** occupies the intermediate slot `XCXN` was specified for. That
> is a reasonable place to be — it reuses the whole existing `ElfParser` → `CXEXWriter`
> packaging path — but the taxonomy and the toolchain disagree, and one of them should move.
> Either introduce `XCXN` as a real CX-native object format (a genuine project, and only
> worth it if there is a reason to stop using ELF as the linkable form), or amend the locked
> chain to name ELF as the intermediate. **Recommendation: amend the chain.** ELF is doing
> the job, the cross toolchain is not going away before self-hosting, and a format invented
> to fill a naming slot is not worth its weight.
>
> **Also inconsistent:** the generated ABI prelude is named `abi.x`, and `.x` is not in this
> taxonomy at all. It is X Native source, so it should be `abi.xfxn`. `CompileCommand`'s doc
> comment likewise says it compiles `.x` files. Small, but this taxonomy only earns its keep
> if the tooling follows it.

**Executables / packages (CXEX-wrapped)**

| Ext | Name | Meaning |
|---|---|---|
| `.XKEX` | X Kernel Executable | Kernel image (System authority) |
| `.XOEX` | X OS Executable | OS/system executable: executive, init, **installer** (System authority) |
| `.XSEX` | X System Executable | System program: owned by the OS but not part of it (shell, supervisor, tools). Platform authority. |
| `.XUEX` | X User Executable | User-space application. Publisher authority, or unsigned by administrator consent. |
| `.XBEX` | X Boot Executable | **Special-purpose**: rewriting boot areas during updates / critical boot fixes only — *not* a routine build output. System authority. |

**Libraries**

| Ext | Name | Meaning |
|---|---|---|
| `.XCDL` | X Common Dynamic Library | Shared/dynamic lib |
| `.XCSL` | X Common Static Library | Static lib |

**Data / system formats**

| Ext | Name | Meaning |
|---|---|---|
| `.XKPK` | X Key, Public | Public key — **carries an Authority header** (§4) |
| `.XKSK` | X Key, Secret | Private key — Authority header; **DevKit key store only**, never in repo |
| `.XFNT` | X Font | Font container (vector or bitmap) — §5.1 |
| `.XCFM` | cX Compiled Font Mask | Editable bitmap-font source drawn in Studio → compiles to `XFNT` — §5.1 |
| `.XFSI` | X icon format | ICO replacement (lib stub now) |
| `.XBPT` | X Boot Partition Table | CX partition scheme |

---

## 4. Key Authority & Signing **[LOCKED — new]**

Keys are not flat: every `XKPK`/`XKSK` carries a **header declaring its Authority domain**, so the system can enforce *who may sign what*. This lets third-party devs sign their own apps while you retain a high-authority root key for System files.

**Authority tiers**

| Tier | May sign | Who holds it |
|---|---|---|
| `ROOT` / `SYSTEM` | `XKEX`, `XOEX`, `XBEX` (and anything below) | CATX (you) — high-security, kept offline |
| `PUBLISHER` | `XUEX`, `XCDL`, `XCSL` | Third-party app developers (via the SDK) |

*(Room to add an intermediate "trusted vendor" tier later — the header field is an enum/flags, not a bool.)*

**Key header (draft fields):** `magic`, `formatVersion`, `authority` (tier enum/flags), `keyId`, `ownerName`, `algorithm`, key material, optional `signedBy` (chain to a higher authority).

**Enforcement [REC, see Q-A]:** CXK verifies an artifact's signature **and** that the signing key's authority tier is permitted for that artifact type (System files require `ROOT`; apps accept `PUBLISHER`). A `PUBLISHER` key signing an `XOEX` is rejected.

> **As-built — the tier is the extension, not a header field.** CXK ships this, and it
> arrived simpler than the draft. A key's authority is stated by its filename: `.xkpk`
> is the platform root compiled into the kernel (`trusted_key.c`) and is the only key
> that may sign a `.xkex`, `.xbex`, `.xoex` or `.xsex`; `.xupk` is a publisher's, lives
> in `/System/KeyVault` on disk, and may sign a `.xuex` and nothing else. So a key's
> ceiling is visible without opening it, and a stolen publisher key cannot be promoted
> by editing a field inside itself. `keyvault.c` answers with `CX_TRUST_PLATFORM`,
> `CX_TRUST_PUBLISHER` or `CX_TRUST_UNVERIFIED`, and `exec.c` compares that against the
> minimum its type demands. The intermediate "trusted vendor" tier of [Q-B] is what a
> vault entry already is: believing a publisher is the act of putting their key there.

**Storage:** private keys (`XKSK`) live in the **DevKit key store** under app data (e.g. `%AppData%/CATX/CXDevKit/keystore/` on Windows; XDG/macOS equivalents). Public keys (`XKPK`) are exported into a project's `…/Keys/`. The SDK ships only `PUBLISHER`-tier keygen/sign (`.xupk` / `.xusk`).

---

## 5. Solution / Library Architecture

**Implemented:** `CXEX.Studio` (2,417 lines), `CXEX.Lang` (1,820), `CXEX.CLI` (1,447), `CXEX.FileSystem` (582), `CXEX.Build` (477), `CXEX.FileType` (364), `CXEX.Disk` (301), `CXEX.Crypto` (279), `CXEX.SDK` (120), `CXEX.Core` (95).

**Scaffolded but empty — project file, zero source:** `CXEX.Font`, `CXEX.ICO`, `CXEX.Text`, `CXEX.Tools`, `CXEX.UI`. Nothing is missing; these are placeholders awaiting the phases in §14. Worth stating plainly because opening the solution gives no hint which libraries are real.

**`CXEX.Tools` [Q-D answered — created, not yet populated]:** the project exists and is empty. The process-tool wrappers (`GccTool`, `NasmTool`, `QemuTool`, `BochsTool`, `CMakeTool`, `ProcessRunner`) still live in `CXEX.CLI/Wrappers`, so Studio cannot drive the toolchain without depending on the CLI. Relocating them is Phase 2 and remains the right call — the compile pipeline already calls `GccTool` from inside `CompileCommand`, which is exactly the coupling this fixes.

**`CXEX.Lang` is the component this section under-describes.** It is the second-largest project and the one everything downstream depends on, yet the table below never mentions it. Its internal pipeline (Lexer → Parser → Sema → CodeGen) is documented on the kernel side in `CXK/docs/CX_X_CORE_LANG.md` §7, because the language spec and the ABI it compiles against live there. That split is deliberate but it has a cost — see §5.2.

| Library | Purpose |
|---|---|
| `CXEX.Disk` | MBR, GPT, XBPT (read/write + viewer models); ISO 9660 + UDF when ISO distribution lands |
| `CXEX.UI` | Shared Avalonia theme + controls (hex view, tree, console, dock chrome) for Studio **and** SDK |
| `CXEX.Text` | UTF-8 / ASCII encoding helpers — for tooling/**interop reading** (other platforms reading our formats). Kernel-side C UTF-8 is a separate question (Q5). |
| `CXEX.Font` | `XFNT` / `XCFM` parsing + generation |
| `CXEX.Tools` | Shared process-tool wrappers + runner (toolchain + emulator launch) |

### 5.1 Font formats **[LOCKED]**

- **`XFNT`** — the deliverable **font container**: `magic` + header describing whether contents are **scalable vector** or **bitmap**, plus name; if bitmap, the **supported sizes** and glyph metrics; then glyph data.
- **`XCFM`** — editable **source** mask: glyphs **drawn on a character map in Studio's font editor**, exported/compiled to a bitmap `XFNT`. (Mirrors the `XF*`→`XC*` source→compiled idea.)
- **`cxk font` (CLI) ingests:** TTF/OTF → vector `XFNT`; BDF (a bitmap-font container — confirmed) and PNG/BMP glyph sheets → bitmap `XFNT`; `XCFM` → bitmap `XFNT`.
- **PNG/BMP glyph-sheet convention:** white = background, black = glyph; each character cell padded 1px on all sides (a clean pixel array).

---

### 5.2 The cross-repo ABI coupling **[known hazard]**

The kernel's syscall ABI is defined in `CXK/abi/cxk_abi.h`. The compiler needs it as X source, and carries it as `CXEX.Lang/Abi/AbiPrelude.cs`, prepended to every compilation.

**That file emits a header reading `GENERATED from cxk_abi.h — Do not edit by hand`, and nothing generates it.** It is a hand-written C# string literal, in a different repository from the header it claims to track, with no build step or test between them.

This is not hypothetical. The kernel gained `SYS_MOUSE_READ` and `struct mouse_state`; the prelude did not; `gui.xfxn` referenced both; and the committed compiler could not compile the committed OS. The drift was exactly one syscall and one struct — small enough to be invisible, fatal enough to break the build.

### Mitigation, as built

`CXEX.Lang/Abi/AbiSync.cs` compares the two and `cxk check-abi` runs it. CXK's `tools/build.bat` invokes it as a pre-flight next to the existing `cxk check`, so drift fails the build early and legibly. The prelude's banner no longer claims to be generated; it says it is hand-maintained and points at the check.

**The family rule** is what makes this usable. The prelude mirrors only part of the header — `SYS_*`, `E_*`, `POWER_*`, `FB_OP_*` — and not `CAP_*` or `NET_OP_*`. Demanding total parity would report sixteen false positives on the first run and promptly be ignored, which is worse than no check at all. So: *a family with at least one member in the prelude must be complete; a family with none is reported as information.* Add one `NET_OP_` constant and the other six become required.

It checks three things, in ascending order of nastiness: a constant present in the header and missing from a mirrored family; a constant whose **value** differs; and a struct whose **field order** differs. The last is the worst failure available here — a reordered struct compiles on both sides and silently corrupts every call that uses it.

Still outstanding:

1. **A test project.** The comparison is in a library so `dotnet test` can call it, and there is still **no test project in this repository at all**. Until there is, the check depends on someone running the build script.
2. **Generate it.** A build step emitting `abi.x` from the header, making the old banner true. Requires both repos visible at build time, which is why the check came first.

`AbiPrelude.cs` therefore remains a **manual sync point** — but now a declared and verified one. Whoever changes `cxk_abi.h` changes the prelude in the same pass, and `cxk check-abi` says so if they forget.

**The drift surface is wider than this one file.** `os/std/net.xfxn` redeclares all seven `NET_OP_*` constants locally, because the prelude does not carry them — a third copy of the ABI, in a third repository location, with no link back to the header. `cxk check-abi` reports unmirrored families partly to make that visible. The same applies to the `CAP_*` bits, the CXEX header offsets in `CXEX.Build`, and the CXFS layout in `CXEX.FileSystem`.

The same hazard applies to anything else duplicated across the two repos: `CAP_*` bits (already double-declared in `caps.h` and `cxk_abi.h`, but at least those two are static-checked against each other on the kernel side), CXEX header field offsets in `CXEX.Build`, and the CXFS on-disk layout in `CXEX.FileSystem`. Each is a copy of a kernel-side truth with no mechanical link back to it.

---

## 6. The Studio Application

### 6.1 Docking model **[LOCKED]**

- **Locked by default;** drag/rearrange only in **Window Editor Mode** (toggle). Prevents accidental layout destruction.
- **Preset layouts shipped** (*Kernel Dev*, *Disk & Image*, *Debug*, *Minimal*) **+ user-saved** layouts, settable as **either global or per-project** (both supported).
- **Project Explorer is non-closable**, resize-only; **other Tools may share its pane** (tab alongside it) — its placement (left pane vs. tabbed elsewhere) is a user choice.
- **Min width/height** enforced on all panels.

### 6.2 Window inventory & file associations

Windows: Project Explorer (pinned), Build Configuration, Text Editor, Hex Viewer, Image Viewer/Editor, CXFS Browser, Partition Viewer, Key Manager, Settings, Bottom Console Host.

**"Open with" / associations:** extension → default window, with right-click **Open With** override. Context menu also: **Rename, Delete, Copy, Paste, New File/Folder, Lock/Unlock**. Locked files/dirs are read-only in the DevKit (blocks edit+delete), tracked in `settings.json.locks`.

### 6.3 Bottom = multi-use Console Host **[LOCKED]** *(building moves out — it's a logger here)*

A tabbed/selectable console host with a **stream selector** and **context-aware auto-switch**:

| Stream | Content |
|---|---|
| **Build Log / Output** | Build pipeline output — logger, read-only |
| **Emulator Output** | QEMU/Bochs guest **serial (COM1)** — kernel debug |
| **Terminal** | A **real interactive shell**: bash on Linux, pwsh on Windows, the mac equivalent on mac |

Auto-switch on context: starting a build surfaces Build; launching the emulator surfaces Emulator. (Real shell = a PTY-backed terminal control; flagged as a real component to source.)

### 6.4 Hex Viewer

- **Magic detection + highlight** (`CXEX.FileType`) — magic bytes get the pink semantic highlight + label.
- **Metadata panel:** CXEX header fields; disk-image XBPT/partition info; **CXFS** superblock/entries.
- **ASM region highlighting from the build:** using the linked ELF/map, highlight **entry point, exit/return points, stack setup, section boundaries**.
- **Search:** by hex **or** ASCII; in raw-disk/CXFS mode also **by address**. Encoding via `CXEX.Text`.

### 6.5 Text Editor — **[REC: AvaloniaEdit + TextMate]**

My recommendation: standardize on **AvaloniaEdit** (mature, the de-facto Avalonia code editor) with **TextMate grammars**. Why: we get solid C/ASM highlighting for free, a clean path to a **custom X Native grammar** (`.XFXN/.XFXR/.XFXH`), and full control over a **high-contrast bright theme** matching your §2 preference. Rolling our own editor is a large detour for no near-term gain. Fixes the current LoadFile exception (§11) by binding content properly.

### 6.6 Image Editor

Display PNG / BMP / ICO (+ more). `XFSIFile.cs` lib stub now; format later.

### 6.7 Settings

- Global store (theme, syntax-highlight prefs, toolchain paths, default layout) in app data; **per-project overrides** in `Config/settings.json`.
- **Custom themes:** a theme = named palette (4 brand colors + derived ramp) loaded at runtime; ships "CX Dark".

---

## 7. Emulation & Debug **[LOCKED]**

- **Near term:** launch QEMU/Bochs as a process, capture **serial (COM1) → Emulator Output**. CXK currently does **not** mirror klog to serial — add a small serial-mirror in the kernel (it already collects the log for disk, so wiring a COM1 echo is cheap). No in-window graphical embedding.
- **Custom X emulator (later):** a **host-side X VM** that runs `.XFXN`/`.XCXN` against a **stubbed `cxk_abi.h`** (syscalls → host console/files) so apps preview without booting CXK. User-space preview, not full-system emulation.

---

## 8. Multi-Architecture **[LOCKED]**

**i686 / 32-bit is the live target** (groundwork for the rest). Everything else is a stub: arch registry + `Bin/<Arch>/` dirs + toolchain selection in Build Config, with placeholders for `x86_amd64`, `arm`, `riscv`, `8086`, `8080`, …

---

## 9. Disk, Partitions & Installer

- **`CXEX.Disk`:** MBR + GPT + XBPT models + a **partition-table viewer** window; ISO 9660/UDF later.
- **Installer-as-XOEX [REC — endorse, my idea per Q14]:** the build produces (a) an **installer `XOEX`** (System authority; granted `DISK`/`MEM`/`POWER` caps via the broker) and (b) the **on-disk OS `XOEX`**. Flow: **boot CXK from USB → installer XOEX runs → it partitions the target disk (XBPT) and writes stage1/stage2 + `XKEX` + OS `XOEX` → reboot into the installed OS.** This is the natural fit precisely because an installer needs direct kernel/disk access, which a privileged XOEX on the exokernel already brokers — no special host tooling, the installer *is* a CX program. Disk-setup can be its own `XSEX` invoked by the installer or folded into the XOEX. **Make it a managed build target.**

---

## 10. *(reserved)*

---

## 11. Current Bugs — Triage

| # | Symptom | Cause | Fix | Status |
|---|---|---|---|---|
| 1 | Project Explorer shows empty folders | `TreeViewItem` style never binds `IsExpanded`, so the lazy-loader never fires | Add `<Setter Property="IsExpanded" Value="{Binding IsExpanded, Mode=TwoWay}"/>` to the `TreeViewItem` style in `ProjectExplorerView.axaml` | **FIXED** — the setter is present |
| 2 | TextEditor throws | `LoadFile` builds a throwaway view via `DataTemplates.First(...).Build(this)` (throws on no match; loads into an unshown view) | VM holds `FilePath`/`Content` observable props; real view binds them; delete the reflection hack. Same in `ImageEditorViewModel.LoadImage` | **FIXED** — no reflection hack remains in source |
| 3 | Bottom panel controls overflow | 28px header row < control heights | Header row → `Auto`/~36px; explicit control heights | likely fixed — no 28px row height remains; confirm visually |
| 4 | Image Explorer shares Project Explorer's pane | Both are `Tool`s in one `ToolDock` | Separate dock region / own pane (ties to §6.1) | **OPEN** |
| 5 | Can't open other tooling | Only Dashboard/Emulator/Hex wired to open | Openers registry + menu/explorer entries (§6.2) | **OPEN** — `MainWindowViewModel` still exposes only `OpenDashboard`, `OpenEmulator`, `OpenHexInspector` |

Phase 0 is therefore **mostly done**: bugs 1–3 are closed and only the openers (#5) and the dock split (#4) remain before daily use is unblocked.

---

## 12. SDK (CX SDK) **[LOCKED]**

Separate Avalonia app (not yet built), aimed at third-party developers compiling X programs for CXOS. Shares `CXEX.UI` so it visually matches Studio; effectively a **cut-down Studio** for end users. Includes **`PUBLISHER`-tier key signing** (devs sign their own `XUEX`). Aligns with the license: MIT for what they build, proprietary kernel/OS.

---

## 13. Remaining Open Questions

- **[Q5 — still open]** `CXEX.Text` is confirmed for tooling/interop reading. Do we *also* need a parallel **C UTF-8 implementation in the kernel**, or does CXK stay ASCII for now? (Leaning: defer; add C side only when CXOS needs it.)
- **[Q-A]** Confirm authority enforcement: should CXK **reject at load** any artifact whose signing key tier is below what its type requires (System ⇒ ROOT)? (I've assumed yes.)
- **[Q-B]** ~~Authority tiers: `ROOT` + `DEVELOPER` enough to start, or add an intermediate "trusted vendor" tier now?~~ **Settled.** Two tiers, named `PLATFORM` and `PUBLISHER`, carried by the key's extension. A vendor is trusted by being in the vault, so the third tier is a directory entry rather than a format change.
- **[Q-C]** `XCFM` font editor: confirm the in-Studio "draw glyphs on a character map → export/compile to XFNT" workflow is what you want for the bitmap path.
- **[Q-D — half answered]** `CXEX.Tools` was created but left empty; the wrappers still live in `CXEX.CLI/Wrappers`. The relocation itself is still pending (Phase 2).
- **[Q-E — new]** The `.XCXN` intermediate in §3 does not exist and ELF fills its role. Amend the locked chain to name ELF, or build `XCXN` for real? (Recommendation: amend.)
- **[Q-F — new]** The ABI prelude is a manual cross-repo sync point that has already broken a build (§5.2). Add a test project asserting prelude/header agreement, or a generator? (Recommendation: the test, now; the generator later.)

---

## 14. Roadmap *(your §11 order, adjusted for the new decisions)*

**Phase 0 — Unblock daily use:** ~~bug fixes 1–3~~ **done**; openers for all windows (#5) and the dock split (#4) remain. *(reopenable bottom panel already done.)*

**Phase 0.5 — Close the ABI sync hole (§5.2). Done, with one piece outstanding.** `CXEX.Lang/Abi/AbiSync.cs` compares the header against the prelude and `cxk check-abi` exposes it; CXK's `tools/build.bat` runs it as a pre-flight beside the existing source check, so a drifted prelude now stops the build with a clear message instead of producing confusing "undefined name" errors in `gui.xfxn`. The misleading "GENERATED … Do not edit by hand" banner is gone — the prelude now says it is hand-maintained and names the check.

*Outstanding:* the comparison lives in a library precisely so a test project can call it, and **that test project still does not exist.** `dotnet test` is the natural CI gate; the CLI command is the developer-facing half. Creating it remains the cheapest way to make this automatic rather than build-script-dependent.

**Phase 1 — Identity & shell:** `CXEX.UI` skeleton + **CX Dark** theme; de-VS-Code chrome (Spyder-style icon bar, flat-with-contrast, no palette-only); min sizes; locked docking + Window Editor Mode + preset/custom layouts (global & per-project).

**Phase 2 — Build Config window + `CXEX.Tools`:** relocate tool wrappers to `CXEX.Tools`; move building out of the bottom panel into a flags/config window → pipeline → Build Log; define `Config/settings.json` schema + mounted-disk UI. (Replaces batch/ps1 in earnest.)

**Phase 3 — Console Host:** multi-stream consoles (Build / Emulator-serial / real Terminal) with selector + context auto-switch; add CXK klog→COM1 serial mirror.

**Phase 4 — Hex Viewer:** magic ID + highlight, metadata (CXEX/CXFS/partition), asm region highlighting, hex/ascii/address search; `CXEX.Text`.

**Phase 5 — Explorer power:** context-menu CRUD, open-with/override, file/dir locks.

**Phase 6 — Editor:** AvaloniaEdit + X Native TextMate grammar + high-contrast theme.

**Phase 7 — Keys & signing:** key store (appdata), `XKPK`/`XKSK` authority headers, sign/verify with tier enforcement; Key Manager window.

**Phase 8 — Disk & installer:** `CXEX.Disk` (MBR/GPT/XBPT + viewer); installer-as-XOEX build target.

**Phase 9 — CLI tooling:** `cxk font` (XFNT/XCFM, TTF/OTF/BDF/PNG-BMP) + `CXEX.Font`; multi-arch stubs.

**Phase 10 — Bigger bets:** host-side X emulator (XFXN/XCXN preview); image editor + XFSI; **CX SDK** app on `CXEX.UI`.

---

*End v0.2. Decisions are locked except §13. Say the word and I'll start Phase 0 (bug fixes 1–3) immediately.*