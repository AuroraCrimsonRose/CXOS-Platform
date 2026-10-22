# `cxk`: what comes next
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: PLAN.** The command-line tool as built is `devkit/CXEX.CLI`
> (Spectre.Console.Cli, about 4,100 lines, 25 commands); its architecture is in
> `CX_DEVKIT_DESIGN.md`. This document is what changes and why.

---

## 1. Where it stands

Good already: one tool replaced a pile of scripts; `verify-boot` reads the
serial console and returns PASS, FAIL or INCONCLUSIVE; `check-abi` and
`check-versions` run before every OS build; the build is deterministic.

What hurts:

| Problem | Effect |
|---|---|
| **A stale prebuilt.** `tools/cxk.exe` is gitignored and published by hand | It has been found days out of date; every DevKit change needs a manual republish, and forgetting it is a silent wrong answer until `check-abi` happens to notice |
| **A flat list of 25 commands** mixing compiler internals (`tokens`, `ast`, `sema`, `asm`) with daily work (`os build`) | Hard to find the right one; help text still says what each replaced years ago |
| **Loud output.** An OS build prints about 300 lines, four of which matter | Every session filters by hand; failures hide in the noise |
| **Text-only results** | Scripts, the CI container and agents scrape prose |
| **Sabotage by hand.** Every check must be seen to fail, done by editing a file, rebuilding, booting and undoing | Slow, easy to get wrong (undoing with `git checkout` has destroyed real fixes twice), and never re-run later |
| **x86-32 assumptions:** `dist/CXK_x86_32/`, `qemu-system-i386`, RSA `keygen` | All change with the move |
| **Nothing for the rig:** no way to write a USB stick, read a crash log, or symbolise an address | Real-hardware testing is manual |

## 2. How the others do it

- **Serenity:** `Meta/serenity.sh` wraps build, run, test, debug and GDB per
  target.
- **Rust's `cargo xtask`:** the project's own tool is built from source on
  every use, so it is never stale.
- **Fuchsia's `fx`:** one front door with subcommands (`fx build`, `fx test`,
  `fx log`, `fx symbolize`), and `fx doctor` to check the machine.
- **Zig:** `zig build test`, and a formatter and language server shipped with
  the compiler.

## 3. Changes

### 3.1 Never stale

`tools/cxk` becomes a small launcher that compares a hash of `devkit/` with
the hash the published binary was built from, and rebuilds it first if they
differ (the `cargo xtask` idea). The binary also prints the hash it was built
from in `--version`. The manual republish step and its failure mode disappear.

### 3.2 Grouped commands

Noun first, verb second:

| Group | Commands |
|---|---|
| `cxk os` | `build`, `run`, `verify`, `image`, `clean` |
| `cxk test` | `host`, `boot`, `diff` (in the container), `sabotage`, `fuzz`, `all` |
| `cxk x` | `compile`, `check`, `fmt`, `test`, `lsp`; the internals (`tokens`, `ast`, `sema`, `asm`) under `cxk x debug` |
| `cxk xfil` | `gen`, `check` (the interface generator, `kernel/CX_SYSCALLS_IPC.md` §9) |
| `cxk key` | `gen` (Ed25519), `list`, `revoke`, `rotate` |
| `cxk sign`, `cxk inspect` | as now |
| `cxk check` | `abi`, `versions`, `xdata`, `all` |
| `cxk rig` | `usb` (write the image to a stick), `crashlog`, `symbolize` |
| `cxk doctor` | checks clang 23+, ld.lld, NASM, CMake, Ninja, QEMU, Docker, the keys, and that `cxk` matches the tree |
| `cxk ci` | runs `tools/ci` in the container, with `MSYS_NO_PATHCONV` set for you |

The old names stay as aliases for one release, then go.

### 3.3 Quiet by default, structured on request

- **Summary output by default:** each step on one line with its time; on
  failure, the first error and the path of the full log. The full log always
  goes to `build/logs/`. `--verbose` restores everything.
- **`--json` on every checking command** (`verify`, `check`, `test`, `inspect`):
  one result object, stable field names, so the container, scripts and agents
  read results instead of scraping them.
- **One exit-code meaning everywhere:** 0 pass, 1 fail, 2 inconclusive, 3 the
  tool itself could not run.

### 3.4 Sabotage as a command

The rule that every check is seen to fail becomes a tool:

- Each check registers its sabotage in `tests/sabotage/` (a small patch and the
  tests that must turn red).
- `cxk test sabotage <name>` applies the patch in a scratch copy of the tree,
  builds, runs, checks that **exactly** the named tests failed, and never
  touches the working tree. `cxk test sabotage --all` re-proves every check,
  so a test that quietly stopped testing anything is caught later, not only on
  the day it was written.

This is mutation testing aimed at the checks that matter
(`safety/CX_SAFETY.md` §4, item 4).

### 3.5 Flakes and replay

- `cxk os verify --repeat 50` boots fifty times and reports the failure rate
  and the first failing log.
- `cxk os run --record` and `--replay` drive QEMU's whole-machine record and
  replay, so an intermittent failure (the install bug) can be caught once and
  replayed exactly.
- `cxk os run --gdb` starts QEMU paused with symbols loaded.

### 3.6 Machines

`--cpu` presets drive the CPU-feature tests (`kernel/CX_KERNEL_PLAN.md`
§2.3): `min` (baseline x86-64), `max`, and `rig`. QEMU's `Opteron_G5` model is
Piledriver, so `--machine rig` gives that CPU, 8 GB and AHCI, as close to the
real machine as QEMU gets.

### 3.7 The rig

- `cxk rig usb <disk>` writes the image to a USB stick, refusing any disk that
  holds a mounted volume or is larger than a set size, so the system disk
  cannot be overwritten by a typo.
- `cxk rig crashlog <disk>` reads the crash log saved on the stick
  (`kernel/CX_KERNEL_PLAN.md` §8) and `cxk rig symbolize` turns its addresses
  into function names and lines. **`crashlog` exists since roadmap step 4b**
  for disk images; reading the stick itself (a raw Windows device, which
  needs an administrator) is still to come. `cxk verify-crashlog <image>`
  boots a copy twice, the first asked to panic (through a request record it
  writes into the CRASH partition), and requires the record on disk after the
  panic, then `/System/crash.log` recovered and the area cleared by the second
  boot. With `--usb xhci|ehci|ohci` (step 4c) the copy is a stick on that
  controller, the first boot leaves a READ half done before it panics, and
  the record must say the save reset the stick first. `cxk verify-boot` takes
  the same `--usb`, and `--foreign-disk`: a patterned 32 MB disk ahead of the
  image, as a machine's own drive, which must come out of the boot
  byte-for-byte unchanged (the install guard, `CXK_AUTO_INSTALL`).

### 3.8 Later

- A build cache keyed by content, so a rebuild after a comment change does not
  rebuild the world.
- `cxk` running on CXOS itself, once X can build it: the self-hosting
  milestone for the tools, after the compiler's.

## 4. Order

| When | Work |
|---|---|
| Now | The launcher that is never stale; `--version` with the tree hash; `cxk doctor` |
| Phase 0 | Quiet output and `--json`; `cxk test sabotage`; `--repeat`; `--record`/`--replay` |
| Phase 0.5 | x86-64 targets, `--cpu` and `--machine rig`, `cxk rig usb`, `crashlog`, `symbolize` |
| Phase 0.6 | `cxk xfil`; `cxk key` with Ed25519; the grouped commands |
| Later | `cxk x fmt`, `test`, `lsp`; the build cache; `cxk` on CXOS |
