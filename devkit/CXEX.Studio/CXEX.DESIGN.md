# CX DevKit — Architecture, Vision & Roadmap

**Owner:** Aurora Tejeda · **Company:** CATX Systems LLC · **Products:** CX, CXK, CXOS
**Doc status:** **v0.2** — decisions from the Q&A pass are now **locked** (marked **[LOCKED]**); a handful of genuinely-open items remain in §13.

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
| `.XCXR` / `.XCXH` | (compiled runtime / hybrid) | Same pattern, as those dialects come online |

**Compile/link chain [LOCKED, my call per Q3a]:** `*.XFXN` (source) → **`*.XCXN`** (compiled linkable object) → **packaged executable** (`XCEX` / `XOEX` / `XKEX` depending on target). `XCXN` is the intermediate; the `X_EX` family is the final CXEX-headered, signable artifact.

**Executables / packages (CXEX-wrapped)**

| Ext | Name | Meaning |
|---|---|---|
| `.XKEX` | X Kernel Executable | Kernel image (System authority) |
| `.XOEX` | X OS Executable | OS/system executable: executive, init, **installer** (System authority) |
| `.XCEX` | X Common Executable | User-space application (Developer authority) |
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
| `DEVELOPER` | `XCEX`, `XCDL`, `XCSL` | Third-party app developers (via the SDK) |

*(Room to add an intermediate "trusted vendor" tier later — the header field is an enum/flags, not a bool.)*

**Key header (draft fields):** `magic`, `formatVersion`, `authority` (tier enum/flags), `keyId`, `ownerName`, `algorithm`, key material, optional `signedBy` (chain to a higher authority).

**Enforcement [REC, see Q-A]:** CXK verifies an artifact's signature **and** that the signing key's authority tier is permitted for that artifact type (System files require `ROOT`; apps accept `DEVELOPER`). A `DEVELOPER` key signing an `XOEX` is rejected.

**Storage:** private keys (`XKSK`) live in the **DevKit key store** under app data (e.g. `%AppData%/CATX/CXDevKit/keystore/` on Windows; XDG/macOS equivalents). Public keys (`XKPK`) are exported into a project's `…/Keys/`. The SDK ships only `DEVELOPER`-tier keygen/sign.

---

## 5. Solution / Library Architecture

**Existing:** `CXEX.Build`, `CXEX.Crypto`, `CXEX.Core`, `CXEX.FileSystem`, `CXEX.FileType`, `CXEX.Lang`, `CXEX.CLI`, `CXEX.Studio`.

**Added by you:** `CXEX.Disk`, `CXEX.UI`, `CXEX.Text`, `CXEX.Font`.

**Proposed new:** **`CXEX.Tools` [REC — strong]** — move the process-tool wrappers (`GccTool`, `NasmTool`, `QemuTool`, `BochsTool`, `CMakeTool`, `ProcessRunner`) out of `CXEX.CLI` into a shared lib so **both CLI and Studio** drive the toolchain from one place without coupling to each other.

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
- **Installer-as-XOEX [REC — endorse, my idea per Q14]:** the build produces (a) an **installer `XOEX`** (System authority; granted `DISK`/`MEM`/`POWER` caps via the broker) and (b) the **on-disk OS `XOEX`**. Flow: **boot CXK from USB → installer XOEX runs → it partitions the target disk (XBPT) and writes stage1/stage2 + `XKEX` + OS `XOEX` → reboot into the installed OS.** This is the natural fit precisely because an installer needs direct kernel/disk access, which a privileged XOEX on the exokernel already brokers — no special host tooling, the installer *is* a CX program. Disk-setup can be its own `XCEX` invoked by the installer or folded into the XOEX. **Make it a managed build target.**

---

## 10. *(reserved)*

---

## 11. Current Bugs — Triage *(next work item)*

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | Project Explorer shows empty folders | `TreeViewItem` style never binds `IsExpanded`, so the lazy-loader never fires | Add `<Setter Property="IsExpanded" Value="{Binding IsExpanded, Mode=TwoWay}"/>` to the `TreeViewItem` style in `ProjectExplorerView.axaml` |
| 2 | TextEditor throws | `LoadFile` builds a throwaway view via `DataTemplates.First(...).Build(this)` (throws on no match; loads into an unshown view) | VM holds `FilePath`/`Content` observable props; real view binds them; delete the reflection hack. Same in `ImageEditorViewModel.LoadImage` |
| 3 | Bottom panel controls overflow | 28px header row < control heights | Header row → `Auto`/~36px; explicit control heights |
| 4 | Image Explorer shares Project Explorer's pane | Both are `Tool`s in one `ToolDock` | Separate dock region / own pane (ties to §6.1) |
| 5 | Can't open other tooling | Only Dashboard/Emulator/Hex wired to open | Openers registry + menu/explorer entries (§6.2) |

Bugs 1–3 are small and unblock daily use — first to fix.

---

## 12. SDK (CX SDK) **[LOCKED]**

Separate Avalonia app (not yet built), aimed at third-party developers compiling X programs for CXOS. Shares `CXEX.UI` so it visually matches Studio; effectively a **cut-down Studio** for end users. Includes **`DEVELOPER`-tier key signing** (devs sign their own `XCEX`). Aligns with the license: MIT for what they build, proprietary kernel/OS.

---

## 13. Remaining Open Questions

- **[Q5 — still open]** `CXEX.Text` is confirmed for tooling/interop reading. Do we *also* need a parallel **C UTF-8 implementation in the kernel**, or does CXK stay ASCII for now? (Leaning: defer; add C side only when CXOS needs it.)
- **[Q-A]** Confirm authority enforcement: should CXK **reject at load** any artifact whose signing key tier is below what its type requires (System ⇒ ROOT)? (I've assumed yes.)
- **[Q-B]** Authority tiers: `ROOT` + `DEVELOPER` enough to start, or add an intermediate "trusted vendor" tier now?
- **[Q-C]** `XCFM` font editor: confirm the in-Studio "draw glyphs on a character map → export/compile to XFNT" workflow is what you want for the bitmap path.
- **[Q-D]** `CXEX.Tools`: OK to create it and relocate the tool wrappers + `ProcessRunner` there (shared by CLI + Studio)?

---

## 14. Roadmap *(your §11 order, adjusted for the new decisions)*

**Phase 0 — Unblock daily use:** bug fixes 1–3; openers for all windows (#5). *(reopenable bottom panel already done.)*

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