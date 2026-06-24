# CX DevKit — Architecture, Vision & Roadmap

**Owner:** Aurora Tejeda · **Company:** CATX Systems LLC · **Products:** CX, CXK, CXOS
**Doc status:** v0.1 working draft — captures the full vision dump, locks the decisions that block everything else, and lists the open questions I need answered.

This is a planning document, not a spec freeze. Where I've made a recommendation I've marked it **[REC]**; where I need your call I've marked it **[Q#]** and collected all questions at the end.

---

## 1. Vision

The CX DevKit is the **single, ergonomic platform** for building the CX ecosystem: the CXK kernel, the CXOS operating system, bootloaders, and X-language user/OS executables — across multiple target architectures — replacing the current Python / batch / PowerShell toolchain.

Two faces of the same core:

- **CX DevKit (Studio)** — the full IDE for *us* (kernel/OS/boot/app development, packaging, signing, imaging, debugging).
- **CX SDK (CXEX.UI-based)** — a future, slimmer, user-facing app for *third parties* building X apps for CXOS. Shares a common Avalonia control/theme library (`CXEX.UI`) with Studio so they look and feel like one product.

Design principles: build in small verifiable checkpoints; logic lives in libraries (CLI and Studio are thin front-ends over the same code); the look is **IDE-like but distinctly CX**, not a VS Code clone.

---

## 2. Visual Identity

Moving away from the VS Code aesthetic while keeping a familiar IDE *layout*. The restyle is primarily theme/chrome, not a layout teardown.

**Brand palette**

| Hex | Role **[REC]** | Notes |
|---|---|---|
| `#111827` | Primary surface (deep navy-black) | Main background, darkest layer |
| `#5BC0F8` | Primary accent (CX cyan) | Active states, selection, primary buttons, focus |
| `#F48FB1` | Secondary accent (CX pink) | Highlights, warnings-as-attention, secondary emphasis, magic-byte highlight |
| `#EAF4FF` | Foreground (near-white blue) | Primary text/icons on dark surfaces |

I'd derive a small ramp from `#111827` (e.g. `#0B0F19` / `#111827` / `#1B2433` / `#2A3650`) for layered surfaces, panel borders, and dividers, so it reads as a designed dark theme rather than flat gray. Accent cyan for interaction, pink reserved for "look here" semantic highlights (magic bytes, entry points). **[Q1]**

**Distinct-from-VS-Code moves [REC]:** replace the left activity bar + thin status bar paradigm with a CX-branded top bar (wordmark + global actions), softer rounded panel chrome, accent-tinted active tabs, and a custom title/menu treatment. Layout stays a docked IDE; the *chrome* is what changes. Needs a reference direction from you — see **[Q2]**.

---

## 3. Artifact & File-Type Taxonomy *(foundational — blocks tooling, must lock first)*

Everything downstream (packaging, project layout, explorer icons, "open with", build targets) depends on a frozen taxonomy. Here's my current understanding; **please correct/confirm — this is the highest-priority decision [Q3]**.

**Executables / packages (CXEX-wrapped)**

| Ext | Name | Meaning |
|---|---|---|
| `.XKEX` | X Kernel Executable | The kernel image |
| `.XOEX` | X OS Executable | OS/system executable (executive, init, installer) |
| `.XBEX` | X Boot Executable | Bootloader artifact *(new — confirm boot becomes CXEX-family)* |
| `.XCEX` | X Common Executable | User-space application |

**Libraries**

| Ext | Name | Meaning |
|---|---|---|
| `.XCDL` | X Common Dynamic Library | Shared/dynamic lib |
| `.XCSL` | X Common Static Library | Static lib |

**Source & compiled X**

| Ext | Name | Meaning |
|---|---|---|
| `.X` | X (Native) source | Systems core dialect |
| `.XR` | X Runtime source | Runtime dialect (lowers to X core) |
| `.XC` | Compiled X | Object/container after compile — **relationship to XCEX needs definition [Q3a]** |

**Data formats**

| Ext | Name | Meaning |
|---|---|---|
| `.XKPK` | X Key, Public | Public signing key *(private = `.XKSK`? [Q4])* |
| `.XFNT` | X Font ("cX File foNT") | Custom font format |
| `.XFSI` | X icon format | Replacement for Windows `.ICO` (lib stub now) |
| `.XBPT` | X Boot Partition Table | CX partition scheme |

**Open structural questions:** Is `XC` the raw compiled object and `XCEX` the *packaged+CXEX-headered* executable (i.e. `X → XC → XCEX`)? Does the boot path now produce `XBEX`, or do stage1/stage2 stay raw inside the XBPT image? **[Q3a, Q3b]**

---

## 4. Project Model

DevKit-created projects assume an OS build and use this layout (from your spec):

```
$root$
├── Kernel/        (*.asm *.c *.h …)
├── Boot/          (*.asm *.c *.h …)
├── Executable/    (*.asm *.c *.h *.x *.xr …)
├── Config/        (bochsrc.txt, CMakeLists.txt, settings.json, linker configs …)
├── Bin/<Arch>/<Debug|Release>/
│     └── Release/ → Images/  Packages/{Executables,Apps,Libraries}/  Keys/
└── Temp/Artifacts/   (disposable: cmake files, .o, generated, clean-buildable)
```

**Per-project `Config/settings.json` [REC]** — the project's single source of truth. Proposed schema (draft):

```jsonc
{
  "name": "CXOS",
  "company": "CATX Systems LLC",
  "formatVersion": 1,
  "targets": {
    "kernel": { "arch": "i686", "sources": "Kernel/", "linker": "Config/kernel.ld", "type": "XKEX" },
    "boot":   { "arch": "i686", "sources": "Boot/",   "type": "XBEX" },
    "os":     { "sources": "Executable/", "type": "XOEX" },
    "apps":   { "sources": "Executable/", "type": "XCEX" }
  },
  "toolchain": { "gcc": "i686-elf-gcc", "nasm": "nasm", "qemu": "qemu-system-i386", "bochs": "bochs" },
  "mountedDisk": { "image": "Bin/i686/Release/Images/cxk_disk.img", "autoMount": true },
  "keys": { "public": "Bin/i686/Release/Keys/Public.XKPK", "private": "..." },
  "build": { "config": "Debug", "flags": { "quietBoot": true, "serialDebug": true } },
  "locks": ["Kernel/cxfs.h", "Config/"],   // anti-accidental-edit protection
  "ui": { "layout": "Kernel Dev", "theme": "CX Dark" }
}
```

**Mounted disk [REC]:** the project pins one (or more) disk images in `settings.json`/UI so the Hex viewer, CXFS browser, and partition viewer can reference it directly without re-navigating the explorer each time. A "Mounted Disk" selector lives in the top bar.

---

## 5. Solution / Library Architecture

**Existing (keep, all the real logic):** `CXEX.Build` (ELF→CXEX, disk imaging), `CXEX.Crypto`, `CXEX.Core`, `CXEX.FileSystem`, `CXEX.FileType`, `CXEX.Lang` (X compiler), `CXEX.CLI`, `CXEX.Studio`.

**New libraries proposed**

| Library | Purpose |
|---|---|
| `CXEX.Disk` **[REC]** | MBR, GPT, XBPT (read/write + viewer model); ISO 9660 + UDF later for ISO images |
| `CXEX.UI` **[REC]** | Shared Avalonia theme + controls (hex view, tree, console, dock chrome) for Studio **and** the future SDK |
| `CXEX.Text` **[REC]** | UTF-8 / ASCII encoding helpers. **Note:** the kernel is freestanding C and cannot consume a .NET lib — if CXK needs UTF-8 that's separate C. This lib is for the *tools*. **[Q5]** |
| `CXEX.Font` (or in CLI) | XFNT generation/parsing |

**CLI additions**

- `cxk font` — TTF/OTF/BDF/bitmap-PNG → `.XFNT` (header: magic, name, scalable flag, bitmap flag, size list, glyph data). **Scalable semantics need definition [Q6].**
- `cxk disk` — inspect/build MBR/GPT/XBPT (wraps `CXEX.Disk`).
- Multi-arch target flags (see §8).

---

## 6. The Studio Application

### 6.1 Docking model

- **Docked layout, locked by default [REC].** Users can't drag/rearrange panels unless they enter **Window Editor Mode** (a toggle). Prevents accidental layout destruction — a real pain point in Dock-based IDEs.
- **Preset layouts shipped built-in [REC]:** e.g. *Kernel Dev*, *Disk & Image*, *Debug*, *Minimal*. Plus **user-saved custom layouts**, persisted (global and/or per-project — **[Q7]**).
- **Project Explorer is pinned** to its location (left), non-closable, resize-only; other Tools *may* be tabbed alongside it. **[Q8]**
- **Minimum width/height** enforced on all panels (`MinWidth`/`MinHeight`), so nothing collapses to an unusable sliver.

### 6.2 Window / panel inventory

| Window | Kind | Purpose |
|---|---|---|
| Project Explorer | Tool (pinned, left) | File tree, context menu, locks |
| Build Configuration | Document (center) | Set flags/config per target, then trigger build → logs to bottom |
| Text Editor | Document | Code editing w/ syntax highlighting incl. X Native |
| Hex Viewer | Document | Magic ID, metadata, asm region highlight, search |
| Image Viewer/Editor | Document | PNG/BMP/ICO + XFSI |
| CXFS Browser | Document/Tool | Browse CXFS inside a mounted disk |
| Partition Viewer | Document | XBPT/MBR/GPT layout |
| Key Manager | Document | Keygen/sign via `CXEX.Crypto` |
| Settings | Document/Dialog | Global prefs (theme, highlighting) |
| Bottom Console Host | Tool (bottom) | Multi-tab consoles (see 6.3) |

**"Open with" / file associations [REC]:** a registry mapping extension → default window, with a right-click **Open With** override. Right-click context menu also provides **Rename, Delete, Copy, Paste, New File/Folder**, and **Lock/Unlock**. Locked files/dirs are read-only in the DevKit (blocks edit + delete) — cheap accidental-damage protection, tracked in `settings.json.locks`.

### 6.3 Bottom panel = multi-use console host *(redesign)*

Today it's a single build panel that doubles as the builder. New model **[REC]**: the bottom is a **tabbed console host**, and building moves to the Build Configuration window.

| Tab | Content |
|---|---|
| **Build Log** | Logger only — streamed output from the build pipeline (read-only) |
| **Output** | Generic tool stdout/stderr (gcc/nasm/cmake) |
| **Serial Debug** | QEMU/Bochs guest serial (COM1) — for kernel debug **[Q9: confirm COM1/serial]** |
| **Terminal** | A real interactive shell (pwsh/bash). **True PTY is a real component — MVP or later? [Q10]** |

### 6.4 Hex Viewer (high-value, detailed)

- **Magic detection + highlight** via `CXEX.FileType`; the detected magic bytes get an accent (pink) highlight + a label.
- **Format metadata panel:** for CXEX family show header fields; for disk images show XBPT/partition info; for CXFS show superblock/entries.
- **ASM region highlighting from the build:** using the linked ELF/map, highlight entry point, exit/return points, stack setup, section boundaries — correlate file offsets to symbols.
- **Search:** by hex pattern **or** ASCII. In raw-disk/CXFS mode, also **search by address**.
- Encoding via `CXEX.Text` (UTF-8/ASCII rendering of the byte pane).

### 6.5 Text Editor

- Syntax highlighting; **custom "X Native" highlighting** for `.X`/`.XR` (keywords, types, `__syscall`, etc.). Avalonia options: AvaloniaEdit (TextMate grammars) is the pragmatic path — write an X TextMate grammar. **[Q11]**
- Fixes the current exception (see §10).

### 6.6 Image Editor

- Display PNG / BMP / ICO (and others). **XFSI** added as a lib stub now (`XFSIFile.cs`), format defined later.

### 6.7 Settings

- Global settings store (theme selection, syntax highlighting prefs, toolchain paths, default layout). Likely `%AppData%/CATX/CXDevKit/settings.json` + per-project overrides.
- **Custom themes [REC]:** theme = a named palette (the 4 brand colors + derived ramp) loaded at runtime; ships with "CX Dark", user can add.

---

## 7. Emulation & Debug

**Near term [REC]:** launch QEMU/Bochs as a process, capture **serial → Serial Debug console** (already have `EmulatorService`). True in-window graphical embedding isn't portable — skip.

**Later — custom X emulator [REC]:** a host-side **X VM** that emulates the CXK syscall ABI (`cxk_abi.h`) so `.X`/`.XC` programs run/preview *without booting CXK*. Interpret the typed AST (or the emitted asm) and stub syscalls to host console/files. Great for fast app iteration. **Confirm scope: user-space X preview, not full-system emulation [Q12].**

---

## 8. Multi-Architecture

Stubs now, real codegen later. **[REC]:**

- Project `Bin/<Arch>/` already encodes this; add an **arch/toolchain selector** in Build Config.
- Architecture registry with stubs: `x86_amd64`, `i386`/`i686` (real today), `arm`, `riscv`, `8086`, `8080`, …
- X backend is x86-32 only today. **Which arch is the real near-term second target vs. pure placeholder? [Q13]** (e.g. is "x86_amd64" a real 64-bit goal soon, or is i686 the only live target for now?)

---

## 9. Disk, Partitions & Installer

- **`CXEX.Disk`:** MBR + GPT + XBPT models, read/write, plus a **partition-table viewer** window. ISO 9660 + UDF added when you move to ISO distribution.
- **Installer-on-USB concept [REC — elegant, endorse]:** build produces (a) an **installer XOEX** (+ a disk-setup **XCEX**) that runs from the USB, and (b) the **on-disk OS XOEX**. The installer sets up the target disk (partition via XBPT, write boot, copy OS), then the machine reboots into the installed OS. This makes the installer a first-class **CXK build target the Studio manages**. **Confirm it's a managed build target [Q14].**

---

## 10. Current Bugs — Triage *(accurate diagnoses from the uploaded build)*

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | **Project Explorer shows empty folders** | `TreeViewItem` style never binds `IsExpanded`, so the lazy-loader (fires on `FileTreeNode.IsExpanded`) never runs — dirs keep only the placeholder | Add `<Setter Property="IsExpanded" Value="{Binding IsExpanded, Mode=TwoWay}"/>` to the `TreeViewItem` style in `ProjectExplorerView.axaml` |
| 2 | **TextEditor throws** | `LoadFile` does `DataTemplates.First(...).Build(this)` on a *throwaway* view; `.First` throws when no match, and even when not, it loads into a view that's never shown | Make `TextEditorViewModel` hold `FilePath`/`Content` observable props; bind the real `TextEditorView` to them; delete the reflection hack. Same bug in `ImageEditorViewModel.LoadImage` |
| 3 | **Bottom panel controls overflow** | Header row is `28px`; TextBox/ComboBox/Button implicit heights exceed it | Header row → `Auto` (or ~36px) with explicit control `Height`s |
| 4 | **Image Explorer shares Project Explorer's pane** | Both are `Tool`s in the same left `ToolDock` → tabbed together ("same square") | Give Image Explorer its own dock region, or make it a Document — ties to docking redesign **[Q8]** |
| 5 | **Can't open other tooling** | Only Dashboard/Emulator/Hex have open commands; CXFS/Partition/KeyManager/etc. have no open path | Add an **openers registry** + menu/explorer entries (part of §6.2) |

I can fix 1–3 immediately on the next pass (they're small and unblock daily use); 4–5 fold into the docking/window-inventory work.

---

## 11. Proposed Roadmap *(you set the order — this is my [REC])*

**Phase 0 — Unblock daily use (small):** bug fixes 1–3; reopenable bottom panel (done); wire openers for all windows (#5).

**Phase 1 — Identity & shell:** `CXEX.UI` skeleton + CX Dark theme (the 4 colors), de-VS-Code chrome, min sizes, locked docking + Window Editor Mode + preset layouts.

**Phase 2 — Build Config window:** move building out of the bottom panel; flags/config UI → pipeline → Build Log. Define `settings.json` schema. Begins replacing batch/ps1 fully.

**Phase 3 — Hex Viewer:** magic ID + highlight, format metadata, asm region highlighting, hex/ascii/address search; `CXEX.Text`.

**Phase 4 — Explorer power:** context menu (CRUD), open-with/override, file/dir locks, mounted-disk UI.

**Phase 5 — Editor:** AvaloniaEdit + X Native grammar.

**Phase 6 — Disk & installer:** `CXEX.Disk` (MBR/GPT/XBPT + viewer), installer-as-XOEX target.

**Phase 7 — CLI tooling:** XFNT font tool, multi-arch stubs, key store.

**Phase 8 — X emulator** (host-side preview VM) and image editor / XFSI.

---

## 12. Open Questions *(what I need from you)*

**Blocking everything:**
- **[Q3]** Confirm the full file-type taxonomy in §3. **[Q3a]** Is the chain `X → XC (compiled) → XCEX (packaged)`? **[Q3b]** Does boot produce `XBEX`, or stay raw inside XBPT?
- **[Q4]** Private key extension — `.XKSK`? Where do private keys live (never in repo)?

**Identity:**
- **[Q1]** Confirm the color→role mapping in §2. Any logo/wordmark asset? Preferred UI font (vs. mono)?
- **[Q2]** A reference direction for "not VS Code" — Rider / Zed / Godot / Blender / fully bespoke? Even one screenshot or "I like X about Y" helps enormously.

**Architecture & scope:**
- **[Q5]** `CXEX.Text` — for tools only, or does the kernel need a parallel C UTF-8 impl?
- **[Q6]** XFNT "scalable" semantics: vector outlines retained, or bitmap resampling? What source formats must it ingest (TTF/OTF/BDF/PNG)?
- **[Q13]** Which architecture is the real near-term 2nd target vs. pure stub? Is "x86_amd64" a soon goal?

**Studio behavior:**
- **[Q7]** Preset/custom layouts: global, per-project, or both?
- **[Q8]** Project Explorer: strictly pinned/non-closable, or may other tabs share its pane? Should Image Explorer be separate?
- **[Q9]** Serial debug: confirm QEMU/Bochs guest output is COM1 serial; does CXK currently mirror klog to serial, or should I add that to the kernel?
- **[Q10]** Bottom "Terminal" tab: real interactive PTY shell (MVP), or just output streams for now?
- **[Q11]** Editor: OK to standardize on AvaloniaEdit + a TextMate grammar for X Native?

**Bigger bets:**
- **[Q12]** Custom X emulator = host-side user-space preview (run `.X`/`.XC` against a stubbed ABI), correct?
- **[Q14]** Installer-as-XOEX: make it a managed CXK build target?
- **[Q15]** `CXEX.UI` + separate SDK: is the SDK a distinct app sharing controls, or Studio in a "simplified mode"? Who's the SDK audience (third-party X-app devs)?
- **[Q16]** Priority: accept the Phase order in §11, or reorder? What's the **one** thing you want working next?

---

*End of draft. Answer whichever questions you have opinions on and I'll lock the relevant sections and start the corresponding phase — Phase 0 bug fixes are ready to go the moment you say so.*