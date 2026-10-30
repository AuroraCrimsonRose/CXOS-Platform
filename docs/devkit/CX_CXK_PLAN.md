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
| **x86-32 assumptions:** `dist/CXK_x86_32/`, `qemu-system-i386`, RSA `keygen` | All change with the move. The first two went in roadmap step 6d3b: `dist/CXK_x86_64/` and `qemu-system-x86_64` are the defaults, and `--arch x86_32` boots a retired image |
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
| `cxk test` | `host`, `boot`, `diff` (in the container), `sabotage`, `fuzz`, `all` (`all` and `sabotage` built in 1.5d3, §3.4) |
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
  read results instead of scraping them. First built: `verify-boot --json`
  (roadmap 1.5d1), the verdict and every self-test's result read from the
  kernel's `@ktest` lines; stdout carries the object alone, errors go to
  stderr.
- **One exit-code meaning everywhere:** 0 pass, 1 fail, 2 inconclusive, 3 the
  tool itself could not run.

### 3.4 Sabotage as a command

The rule that every check is seen to fail becomes a tool. **Built in 1.5d3**
(`devkit/CXEX.Tools/Sabotage.cs`, `TreeGuard.cs`; the command
`TestSabotageCommand.cs`):

- Each step records its sabotages in `tests/sabotage/<step>.sab`: the edits
  and the self-tests that must turn red.
- `cxk test sabotage <file>` first builds and boots the tree as it is (the
  baseline must pass, or nothing can be judged), then for each sabotage, or
  each group of them: applies the edits, runs `os build`, boots with
  `verify-boot --full --json`, restores, and judges by the `@ktest` results.
  `--all` runs every file, so a test that quietly stopped testing anything is
  caught later, not only on the day it was written. `--dry-run` applies and
  restores without building; `--only a,b` picks sabotages.
- **Verdicts:** CAUGHT (red at every test it named, and nothing red that no
  sabotage in the run named), GREEN (the boot passed: nothing covers it), RED
  ELSEWHERE (red, but not where it said, or also somewhere unexplained),
  INCONCLUSIVE, NOT APPLIED (an edit's text did not occur exactly once),
  BUILD FAILED (a failed build, or one that left an old image or kernel: never
  read as red). Exit 0 only if every sabotage was caught; 2 when the tool
  could not judge (baseline red, restore failed, the tree changed).
- **In the working tree, not a copy.** The plan was a scratch copy; but the
  build reads gitignored inputs (the signing keys, `tools/cxk.exe`, the CMake
  cache), and a copy either duplicates those or builds something else, and
  rebuilds from nothing each time. Instead each file is backed up once per
  run (two edits to one file restore the original), under
  `build/sabotage/backup/` with a manifest written before the file changes,
  so a run that dies is restored by the next one; each restore checks the
  file's hash, and the tree's state as git sees it is compared after every
  run. **Edit nothing in the tree while it runs**: the comparison stops it.

The file format, one block per sabotage; text between the markers is taken
exactly as written, with no escapes (C's `||`, `~` and `#` each broke a
hand-written matrix's delimiter):

```
# a comment
[sync-owner]
expect = x: sync: a mutex's word is its owner @ step 21
group = d2
edit = os/std/sync.xfxn
<<<
    if ((__atomic_load(&m.w) & ~FUTEX_WAITERS) != me) { return E_ACCESS_DENIED; }
===
    if (me == 0xFFFF_FFFF) { return E_ACCESS_DENIED; }
>>>
```

- `expect = <test>` names a self-test as the `@ktest` line does; `@ <text>`
  also requires one of its logged lines to contain the text (its step); a
  trailing `*` matches a prefix (`x: *` for the tests a stopped runner
  reports "not run"). At least one per sabotage.
- `expect = hang`: the sabotage removes a wakeup or a deadlock refusal, so
  being caught means the boot hangs (INCONCLUSIVE). Booted alone, never in a
  group.
- `group = <g>`: shares one build and boot with the rest of the group. Only
  for sabotages that land in different tests (CLAUDE.md, efficiency 5).
- `boot = <args>`: more `verify-boot` arguments (`--cpu max`).
- `edit = <path>`, then `<<<`, the old text, `===`, the new, `>>>`: any
  number per sabotage, applied in order.

It judges boots only: a sabotage of the X compiler is judged by the
container's Differential run, still by hand. This is mutation testing aimed
at the checks that matter (`safety/CX_SAFETY.md` §4, item 4).

### 3.4.1 `cxk test all`

Built in 1.5d3 (`TestAllCommand.cs`): `os build --key test`, then
`verify-boot --full --json` plain and with `--cpu max`, then `dotnet test`
on the solution (the DevKit's tests, and the kernel host tests inside it,
read from its TRX file, not the console), then the container's Differential
run (HANDOFF's command, TRX again). One after another, never two at once.
Each step is PASS, FAIL or SKIPPED (docker missing, a toolchain missing,
`--skip`); a skip is never a pass. Summary: `build/test-all/summary.json`.
Exit 0 only when every step ran and passed, 1 when any failed, else 2.

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
  into function names and lines. **`crashlog` exists since roadmap step 4b**,
  and reads the stick itself as well as an image: `cxk rig disks` lists the
  disks (no elevation needed), `disk5` names one, and reading it needs an
  administrator prompt. Whole disks are opened read-only and read in whole
  sectors (`CXEX.Disk/RawDisk.cs`). `cxk rig bootlog <disk>` reads the system log a boot
  saves in the same CRASH partition (sector 64 on, kind 4, rewritten while the
  machine idles), for a machine with no serial port or framebuffer. `cxk rig dump <disk> <out.img>` copies a
  disk's CXOS part, as far as its XBPT table reaches, into an image file for
  the other tools. `cxk verify-crashlog <image>`
  boots a copy twice, the first asked to panic (through a request record it
  writes into the CRASH partition), and requires the record on disk after the
  panic, then `/System/crash.log` recovered and the area cleared by the second
  boot. With `--usb xhci|ehci|ohci` (step 4c) the copy is a stick on that
  controller, the first boot leaves a READ half done before it panics, and
  the record must say the save reset the stick first. `cxk verify-boot` takes
  the same `--usb`, and `--foreign-disk`: a copy of the image under another
  XBPT disk id, with 8 MB of pattern after it, ahead of the image - a
  machine's own drive with CXOS on it already, BOOT, SYSTEM and CRASH
  included - which must come out of the boot
  byte-for-byte unchanged (the install guard, `CXK_AUTO_INSTALL`). It must
  also be seen: the kernel logs each disk it leaves read-only by size, and a
  boot that never names this one is INCONCLUSIVE, as is a disk that cannot be
  read back afterwards.

### 3.8 Later

- A build cache keyed by content, so a rebuild after a comment change does not
  rebuild the world.
- `cxk` running on CXOS itself, once X can build it: the self-hosting
  milestone for the tools, after the compiler's.

## 4. Order

| When | Work |
|---|---|
| Now | The launcher that is never stale; `--version` with the tree hash; `cxk doctor` |
| Phase 0 | Quiet output and `--json`; `cxk test sabotage` (done, 1.5d3); `--repeat`; `--record`/`--replay` |
| Phase 0.5 | x86-64 targets, `--cpu` and `--machine rig`, `cxk rig usb`, `crashlog`, `symbolize` |
| Phase 0.6 | `cxk xfil`; `cxk key` with Ed25519; the grouped commands |
| Later | `cxk x fmt`, `test`, `lsp`; the build cache; `cxk` on CXOS |
