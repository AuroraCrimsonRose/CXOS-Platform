# CXK v5 — Port Manifest & Build Plan
### CATX SYSTEMS LLC — planning doc for the v5 rebuild

> **Status: COMPLETE — historical.** Every step of the §5 port order has landed:
> the new boot chain, core bring-up, the process model (now with per-process
> address spaces), storage + CXFS, the CXEX loader with signature verification,
> and the remaining drivers. This document is kept as the record of *how* v5 was
> sequenced and *why* each piece was ported rather than rewritten. It is not a
> current task list — for that see `README.md` and `docs/PROCESS_MODEL.md` §12.
>
> Two items named here did **not** land as described, deliberately or otherwise:
> - **`usb/ohci.c`** was never ported. `disk.c` still has a `DISK_DRV_USB` case
>   that returns `DISK_ERR_NO_DEVICE`.
> - **The shell was rewritten in X rather than ported from v4 C.** §4's plan to
>   "port but trim" `shell.c`/`commands.c` was superseded, and the commands it
>   names (`ringtest`, `whoami`, `sha256`, `rsaverify`) do not exist in the X
>   shell. The kernel self-tests that `ringtest` used to drive now live in
>   `kernel/ktest.c` and run automatically at boot; the crypto commands have no
>   replacement and signature verification is exercised only via `cxex_exec`.

v5 keeps v4's proven, hardware-validated subsystems and rebuilds only what genuinely
needs it: the **boot chain** (`boot.asm` stage 1 → `.xbex` stage 2) and **CXEX-format
loading**. v4 is preserved in a `v4/` folder as a working reference to port from and
diff against. This is a *port + new boot/load path*, NOT a from-scratch rewrite of
solved problems.

---

## 1. What is genuinely NEW in v5 (build fresh)

- **`boot.asm` (stage 1):** minimal boot sector. Loads stage 2 from a known offset
  and jumps to it. Stays tiny and stable (it is the one piece that cannot be safely
  updated in place). No filesystem logic.
- **Stage 2 (`.xbex`):** the real bootloader, in CXEX format. Loads/eventually
  verifies the kernel (`.xkex`) and hands off. This is where boot logic lives and
  where in-place updates happen (verify-before-deploy; see CXFS doc 11.6.2).
- **CXEX loader:** kernel/boot code that consumes the format lib (`lib/cxex`) to
  place sections in memory and transfer control. First target: load a userspace
  `.xuex` as a ring-3 process (testable on the ported process model); boot-time
  kernel loading comes later.
- **New CMake build flow:** the v4 flow flattens `kernel.elf → kernel.bin` and patches
  it into the image at a fixed offset. v5 produces `.xkex`/`.xbex` (CXEX) and the new
  boot chain places stage 2 at an offset. This is real CMake surgery, planned
  deliberately.

## 2. Port WHOLESALE from v4 (clean, validated — carry over close to as-is)

These are new/clean or fully validated; they should come over with minimal change.

**`lib/` — all of it (the foundation):**
`string.c`, `kmath.c`, `sha256.c`, `bignum.c`, `rsa.c`, `cxex.c`
(sha256/bignum/rsa/cxex are brand-new and host-tested; string/kmath are stable.)

**Memory management (`memman/`):** `pmm.c`, `heap.c`, `paging.c`
(paging is a dependency of almost everything below — port early.)

**CPU core (`cpu/`):** `gdt.c` + `gdt_flush.asm`, `int/idt.c`, `int/pic.c`, `fpu.c`

**Process model (`cpu/`):** `sched.c`, `switch.asm`, `usermode.c` + `usermode.asm`, `uid.c`
(the preemptive ring-3 model — EOI ordering, per-process esp0, save slots — all
hard-won and validated. Port intact; do NOT re-derive.)

## 3. Port from v4 (validated drivers — bring over as the new tree needs them)

Order by dependency, not all at once — port a driver when v5 reaches the point of
needing it, so each addition is testable.

- **char/:** `console.c`, `timer.c`, `rtc.c` (console + timer are needed very early)
- **video/:** `vga.c`, `fb.c`
- **input/:** `keyboard.c`
- **bus/:** `pci.c` (prereq for AHCI/e1000)
- **storage/:** `ata.c`, `ahci.c`, `disk.c` (needed for any on-disk CXEX/CXFS work)
- **power/:** `acpi.c`, `power.c`
- **usb/:** `ohci.c`
- **net/:** `netif.c`, `arp.c`, `ip.c`, `icmp.c`, `e1000.c`

## 4. Port + likely revise

- **`filesys/cxfs.c`** — port, but v5 is where the CXFS v2 work (permissions, locks,
  layout, partition model) eventually lands. Bring v1 over; evolve per the CXFS doc.
- **`shell/` (`shell.c`, `commands.c`, `demo.c`)** — port, but trim: the commands list
  has accumulated demos/tests. Bring the useful ones (`ringtest`, `whoami`, `sha256`,
  `rsaverify`, core utilities); leave behind anything vestigial.

## 5. Suggested port ORDER (each step builds + is testable)

1. **Skeleton + new `boot.asm`** — stage 1 that loads a flat stage-2 stub and prints
   something. Proves the new boot chain boots at all.
2. **Core bring-up:** `lib/string`, paging/pmm/heap, gdt, idt/pic, console, timer.
   Get to a booting v5 kernel with a serial/VGA "alive" message + basic shell.
3. **Process model:** port sched/switch/usermode/uid + `lib` crypto/format libs.
   Re-validate `ringtest`, `whoami`, `sha256`, `rsaverify` on v5.
4. **Storage + CXFS:** pci, ata/ahci/disk, cxfs. Re-validate disk read/write.
5. **CXEX loader:** the new piece — load a userspace `.xuex` as a ring-3 process,
   verifying its signature via the ported `rsa`/`cxex`/`sha256`. The v5 payoff.
6. **Remaining drivers** (net, usb, video extras) as needed.
7. **Stage 2 `.xbex` + boot-time kernel loading** — the larger boot/partition effort,
   once the loader and CXFS are solid.

## 6. Principles carried from v4 development

- Build hard features in small, individually-testable checkpoints; validate each on
  real hardware before moving on.
- Don't re-derive solved subsystems — port the validated code.
- Keep `boot.asm` (stage 1) minimal; it is the irreducible-risk, rarely-updated anchor.
- Host-test pure logic (libs, format parsing, crypto) against ground truth before it
  ever runs on hardware.
- Keep v4 intact as a reference to diff against when a ported piece misbehaves.