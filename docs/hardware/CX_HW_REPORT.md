# The hardware report
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: 1.8a done (2026-11-04): the kernel builds the report, prints it
> to serial and saves it to the crash area. 1.8b done (2026-11-04): `cxk hw
> show`, `hw diff` and `rig hwreport` (§5). 1.8c done (2026-11-04):
> Hyper-V's clock read through it, and fixed (§6). 1.8e done (2026-11-05):
> the rig's report read and diffed against QEMU's and Hyper-V's (§7)**
> (`planning/CX_ROADMAP.md`, Phase 1 step 8).

Before SMP trusts the MADT (step 9), and before the rig is trusted at all,
CXK has to say what each machine it boots on told it, in a form two machines
can be compared by. QEMU, Hyper-V and the rig are three firmwares; the
report is how a difference between them is seen rather than guessed at.

---

## 1. What is in it

| Section | Lines | From |
|---|---|---|
| `boot` | firmware, CXBI flags, SMBIOS entry | the loader's handover (`abi/cxk_boot.h`) |
| `cpu`, `cpuid`, `hypervisor` | vendor, family, model, stepping, brand; leaves 0, 1, 6, 7.0, 0x80000000/1/7/8 raw; the hypervisor's signature and leaves 0x40000000/1 | CPUID (`arch/x86/cpureport.c`) |
| `apic` | I/O APIC routing on, local APIC on, this CPU's APIC id | `arch/x86/apic.c` |
| `mem`, `memsum` | every entry of the loader's map; entries, dropped, usable bytes | CXBI |
| `acpi` | the RSDP and both of its checksums; every table the RSDT, the XSDT and the FADT name, with its checksum judged | the firmware, walked again |
| `madt` | the MADT entry by entry | the firmware |
| `pci`, `pcisum` | every function: ids, class, header, subsystem, interrupt line and pin, the six BARs raw | `drivers/bus/pci.c`'s table |
| `clock` | the clock source; whether the TSC is invariant; the TSC's rate the clock uses and where it came from (`hv` stated, `pit` polled, `tick`); the PIT's own measure of it; the tick's rate asked and the interrupts that came; the APIC timer's rate; the PM timer's port and width; the TSC against the ACPI PM timer | `drivers/char/timer.c`, then a 50 ms measurement |

The tables are read again rather than taken from `drivers/power/acpi.c`,
which reads only the RSDT and judges no table's checksum. What the kernel
does with a bad table is unchanged in 1.8a: the report says it is bad.
NT5's ACPI driver stops the boot on a DSDT whose checksum fails
(`base/busdrv/acpi/driver/shared/acpiinit.c`, `ACPI_BIOS_ERROR` after
`ACPILoadTableCheckSum`); Haiku's loader logs an RSDT or XSDT that fails and
uses it (`src/system/boot/platform/bios_ia32/acpi.cpp`, `acpi_check_rsdt`);
Serenity sums each table (`Kernel/Firmware/ACPI/Parser.cpp`,
`validate_table`) and prefers the XSDT from revision 2
(`locate_main_system_description_table`). Whether CXK refuses a bad table
is decided once the rig's report shows what a 2005 AMI BIOS hands over.

The CPU part is raw registers, not decoded features: two reports are
compared bit for bit, and a bit the decoder does not know still differs.

## 2. The form

`kernel/diag/hwfmt.h` is the definition; tested on the host
(`kernel/test/host/test_hwfmt.c`).

- One fact per line: `@hw`, a tab, the section, then its fields, each after
  a tab; LF at the end (serial adds CR, and a blank line between, as the
  `@ktest` lines).
- Numbers are lower-case hex at a fixed width, unless the field is a rate in
  Hz or a count (decimal). Firmware text keeps printable ASCII, turns the
  rest into `.`, drops trailing spaces, and is `-` when nothing is left.
- First line `@hw begin 1` (the form's version). Last line
  `@hw end <lines> <crc>`: how many lines came before it, and the CRC-32
  (`xcrl_crc32`, IEEE) of all their bytes.
- A report that runs out of room (64 KB) drops the line that did not fit and
  every later one, and says `@hw truncated` before the end line, so a cut
  report still checks and is known to be cut.

ACPI table lines are
`acpi table <via> <sig> <phys> <len> <rev> <oem> <oem-table> <oem-rev> <state>`,
`via` one of `rsdp`, `rsdt`, `xsdt`, `fadt`, `xfadt` (a 64-bit FADT pointer
that differs from the 32-bit one); `state` is `ok` or `bad` by the checksum,
`short` (a length under the header's), `unchecked` (a length over 16 MB, not
mapped), `unmapped`, or `nosum` for the FACS. The MADT's entries:
`cpu`, `ioapic`, `iso`, `nmi`, `lapicaddr`, `x2cpu`, `x2nmi`, `other`, and
`bad <offset> <type> <len>` for an entry too short for its type (skipped by
its length) or whose length is under 2 or runs past the table (the walk
stops). A firmware length is never believed past the table.

## 3. Where it goes

- **Serial**, at boot, after ACPI, the interrupt controllers, PCI, the
  clocks and the crash area are up and before the self-tests, a line at a
  time with interrupts off.
- **The crash area**, as XCRL kind 6 at sector 384 (`diag/crashrec.h`), after
  the boot log and before the area's end: the rig may have no serial port,
  and its stick is the only place a report can be carried off on. The
  self-tests' profile has its own record after it (§8).

`cxk verify-boot` fails a boot whose self-tests passed when the serial log
holds no whole report (`HwReport.Check`, `devkit/CXEX.Tools/HwReport.cs`,
the kernel's `hw_check` in C#).

## 4. Tests

- Host (`test_hwfmt.c`): a report round-trips and checks; each kind of
  damage is refused for its own reason; a cut report says so; fields are
  cleaned and fixed-width; table checksums, short and unmapped tables and
  the FACS; both RSDP sums; a MADT with every entry kind, a short entry and
  a zero-length one, after which nothing is read.
- Boot (`ktest_pc.c`, `ktest_platform_hw_report`): this boot's report
  checks; each section is there once; its `mem` and `pci` lines are the
  loader's map and the bus's functions; an RSDP, FADT and (with the I/O APIC
  up) a MADT naming a CPU; the crash area's copy reads back byte for byte;
  the PIT's count measured a rate, the clock's did not come from the tick,
  and the clock's rate and the PM timer's agree within 1%. `ktest_arch.c`
  (`t_stated_tsc_rate`): where Hyper-V's CPUID grants its frequency MSRs,
  the clock is the rate it states (1.8c).
- DevKit (`BootVerdictTests`): `verify-boot` fails a missing, torn or
  unended report, reads one interleaved with other lines, and judges as the
  kernel does.

**Limits and untested.** The `mem` types are the loader's: `boot/cxbi.asm`
folds every E820 type above 5 into reserved, so a type 7 or 12 on the rig
shows as `reserved`, its attribute dword kept. A cut report, a bad table
and an x2APIC MADT are seen on the host only, since QEMU gives none of them;
a 32-bit PM timer and a machine with no RSDP are not seen at all. The UEFI
path and the rig have never made a report.

## 5. Reading reports: `cxk hw`

Built in 1.8b (`devkit/CXEX.Tools/HwDiff.cs`, pure;
`devkit/CXEX.CLI/Commands/HwCommands.cs`; the record reader
`CrashLog.ReadHwReport` in `devkit/CXEX.Disk/CrashLog.cs`).

- A source is a serial log or a saved report (its `@hw` lines, as
  `HwReport.TextFromSerial` finds them), or a disk whose CRASH partition
  holds one: an image file, or the stick (`disk5`, read-only, elevated). A
  file with an XBPT table and a CRASH partition is read as a disk; anything
  else as text. A report that fails `HwReport.Check` is refused.
- **`cxk rig hwreport <disk> [-o file]`** reads kind 6 at sector 384, checks
  the record's CRC and then the report's own end line, and prints it or
  writes the bare text, which `hw show` and `hw diff` read. Exit 0, 1 on a
  damaged record or report, 2 when none is saved.
- **`cxk hw show <source> [-s section] [--raw]`** prints each line as a
  *fact*: a key naming what it is about, then its fields by name
  (`pci 00:03.0  vendor=8086  device=100e ...`), then **notes**, what one
  report says is wrong with its own machine: a cut report, an RSDP or table
  checksum that is not `ok`, a table `short`, `unchecked` or `unmapped`, a
  MADT entry skipped, memory-map entries dropped, a full PCI table, and the
  TSC measured by the tick and by the PM timer more than 1% apart (1.8c's
  Hyper-V case).
- **`cxk hw diff <a> <b> [--all] [--rate-tolerance %]`** compares facts by
  key, not lines by position: a PCI function by its address, a table by
  `via` and signature (a second SSDT is `#2`), a MADT CPU by its UID, an
  override by bus and source, a CPUID leaf by leaf and subleaf. Each
  difference is **expected** or a **finding**. Expected: addresses the
  firmware chooses (the RSDP, the RSDT and XSDT pointers, every table,
  SMBIOS, the local and I/O APIC), a BAR that moved with its type bits
  unchanged (a 64-bit BAR's high dword is all address), a PCI interrupt
  line, the hypervisor's own section and leaves `0x4000xxxx` (present or
  not), and a measured rate within the tolerance (1%). Everything else is a
  finding, a field this tool has no name for included; a CPUID finding says
  which bits. Exit 0 when nothing is a finding, 1 on one, 2 when a report
  could not be read. `--all` lists the expected ones too.

The judgement is this tool's, from what each field is; no reference tree
was queried for it, since none compares two machines' reports.

**Tests:** `HwDiffTests` (10, DevKit): naming and numbering; a report
against itself; each expected kind; BAR type bits and the high half; each
kind of finding and the tolerance; a VM against bare metal; every note; the
record read from its own sectors, torn and misplaced ones refused, its
places checked against `crashrec.h`; and `cxk` end to end (a report off an
image file, a serial log with CR LF and noise, exit codes, `-s`, `--raw`, a
whole record holding a damaged report). Sabotages: 18 of 18 caught, by hand
(the `.sab` runner judges boots only); the table is in the commit. Also run
by hand: a copy of the image booted, its record read with `rig hwreport`,
no difference from the same boot's serial copy, and `--raw` byte for byte.
**Untested:** reading the stick itself (`disk5`), and any report but
QEMU's.

Seen on the way: QEMU TCG booted **without** `-icount` measures the TSC at
5.42 GHz by the tick and 3.80 GHz by the PM timer, 30% apart, and the APIC
timer 89 MHz against 62 MHz under `-icount`. TCG's clocks follow the host
there, so it says nothing about the kernel yet; 1.8c reads Hyper-V's the
same way.

## 6. What Hyper-V's report said (1.8c)

Before: the TSC at 59.8 GHz by the tick and 3.80 GHz by the PM timer, 93.6%
apart. The tick was wrong. It counted interrupts, and the Gen 1 VM delivers
about **60 a second for the 1000 asked** (the host's pace; a Windows host's
default is 64). `timer_calibrate_tsc` measured the TSC over 50 of them, so
every "millisecond" was 15.7 of them; the APIC timer, measured against that
clock, inherited the error. Two PM-timer tests and the report's 1% check
were red.

Now the TSC's rate comes from, in order:

1. **What the hypervisor states**: Hyper-V's `HV_X64_MSR_TSC_FREQUENCY`
   (0x40000022), read only where CPUID 0x40000003 grants it (EAX bit 11,
   EDX bit 8; `arch_cycles_stated_hz`, `arch/x86/apic.c`). XNU's `tsc_init`
   (`osfmk/i386/tsc.c`) takes a VMM's stated rate over a measurement the
   same way; Linux reads this MSR under the same two bits (from memory).
2. **The PIT's count, polled** (`pit_cycles_hz`, `platform/pc/pit.c`):
   channel 2 in mode 0, the count latched and the TSC read with interrupts
   off in each sample, two 50 ms windows that must agree within 1%, as
   Haiku's `calculate_cpu_conversion_factor` (`arch_timer.cpp`) does.
   Serenity's `TSC::calibrate` uses interrupts only to bound the window and
   its source's raw counter to measure it. No interrupt is counted.
3. **The tick's interrupts**, only when the PIT's count gave nothing.

The PIT is measured whatever is used (`tsc-pit-hz`), and the tick counted
across it gives `tick-irq-hz`. On Hyper-V: stated 3800.03 MHz, PM timer
3800.02 MHz, APIC timer 12.4998 MHz (a round 12.5), PIT **0.82-1.12% low**
across five boots. So the emulated PIT is the clock that is off there,
either side of the 1% bound, which is why a stated rate has a test of its
own. `cxk hw show` notes both: the PIT's rate against the PM timer's, and
interrupts that came more than 10% off the rate asked.

Channel 2's gate matters off QEMU: left shut, the count still moved under
QEMU, whose PIT ignores the gate in mode 0, but under Hyper-V it held and
the report test went red. That boot also showed a dead count must be given
up by stillness, not a total: 5 million samples, each three port accesses
that exit to the host, took minutes; now 1000 samples on one value end it.

**Untested:** the two-window agreement, since no boot has had a stall
inside a window; the gate and the PIT on the rig.

## 7. What the rig's report said (1.8e)

Read off the stick with `rig hwreport` (twice, a session apart: the same
but for the clocks, which agree to 0.001%), then `hw diff` against three
boots of the same image: QEMU's default CPU, QEMU `--cpu Opteron_G5` (the
nearest model to the rig's Piledriver), and Hyper-V. Findings: 145, 140 and
133. Most are what a different machine is: its PCI functions (34-45), its
MADT's CPUs (28-32), and each ACPI table's firmware identity (`oem`,
`oem-table`, `oem-rev`; about 20 a pair). What the kernel has to take from
them:

- **APIC IDs do not start at 0.** The BSP's local APIC ID is `0x10`; the
  MADT lists 8 enabled CPUs at `0x10`-`0x17` (UIDs 1-8) and 16 disabled
  entries at `0x88`-`0x97` (flags 0); the I/O APIC is ID `0x18` at
  `fec00000`, GSI base 0. CPUID leaf 1's initial APIC ID says **0**, not
  `0x10`. Step 9 starts APs by the MADT's IDs, skips an entry without the
  enabled bit, and never takes an ID from CPUID or assumes the BSP is 0.
  Today: `lapic_id()` reads the register, MSI is aimed by it, the I/O APIC
  ID is never used, and `parse_madt` reads no CPU entries, so nothing yet
  assumes otherwise.
- **Interrupt overrides:** IRQ 0 → GSI 2 (flags 0) and IRQ 9 level,
  active **low** (`000f`; QEMU's are active high, `000d`). `route_irq`
  honours both bits. QEMU's overrides for 5, 10 and 11 are not on the rig.
- **ACPI 1.0:** RSDP revision 0, RSDT only, AMI firmware (`ACPIAM`). Every
  table's checksum is `ok`; the FACS is `nosum`, which is right, since it has
  none. Tables: FACP, APIC, MCFG, HPET, OEMB, DSDT; no SSDT, no IVRS (no
  IOMMU). What to do with a bad table still has no rig evidence.
- **Clocks:** the TSC at 4.000 GHz by the PIT and by the PM timer, 0.002%
  apart; **invariant** (`80000007` EDX bit 8, `invariant 2`), which no VM
  here reports. The PM timer is 32 bits wide at `0808`, the first 32-bit one
  seen working (QEMU's is 24 bits at `0608`, Hyper-V's at `0408`). The tick
  comes at 1000 Hz, as asked; the APIC timer at 12.5 MHz.
- **Memory:** 10 E820 entries, 3.98 GiB usable (3.25 below 4 GiB, 752 MiB
  above), and the PCI hole from `0xD0000000`, which holds the framebuffer
  (1.8d). The CPU has 48 physical address bits (QEMU's models, 40); the page
  tables take 52, so nothing changes.
- **SMBIOS 0** is the BIOS path, not the rig: only the UEFI stub looks for
  it, and QEMU's BIOS boots say 0 too.
- **PCI:** 27 functions. SB950 SATA in AHCI mode (00:11.0), IDE (00:14.1),
  3 OHCI and 2 EHCI (no xHCI), HD Audio (00:14.2), the W5000 (01:00.0, a
  64-bit prefetchable BAR at `0xD0000000`) and its HDMI audio, an Intel
  AX210 (02:00.0) and an RTL8168 (03:00.0). The tree has drivers for the
  AHCI, OHCI, EHCI and RTL8168 parts; not for the AX210 or HD Audio. The
  report does not say whether they came up: that is the boot log's.
- **CPUID against Opteron_G5:** the rig adds MONITOR, HTT, APERF/MPERF,
  BMI1, the invariant TSC, and in `80000001` SVM, XOP, FMA4, TBM, TOPOEXT
  and the rest of family 15h's extras. The kernel tests none of them but
  the TSC's.
- **The console** (1.8d): minutes to the prompt before; after, 2316 ms of
  drawing for 664 scrolls and 0 ms of serial waits for the whole boot.
  Hyper-V's for comparison: 190 ms drawing, 853 ms serial waits (an
  emulated UART that holds THRE).

**`hw diff` fixed on the way:** bare metal prints `hypervisor -`, one field
to a VM's two, and the second came out a finding ("a field in one report
only") though the hypervisor's section is expected either way (§5). The
hypervisor's rule is now judged first; `HwDiffTests` covers the line as
bare metal prints it, seen red before the fix. The ACPI identity fields
stay findings, as §5 has them.

**Untested:** whether the drivers named above work on the rig's parts;
the 16 disabled MADT entries, until step 9 reads them.

## 8. The self-tests' profile (1.9f)

A test-hook boot prints a second report in the same form after its last
self-test (`kernel/ktest_stress.c`, `ktest_profile_report`): what
`sched_lock`'s contention cost, per CPU, so a QEMU run, a Hyper-V one and
the rig's are set side by side the way their machines are
(`kernel/CX_SMP.md` §4d). Its first line after the begin is `profile`,
which is how `HwReport.TextFromSerial` passes over it and
`ProfileFromSerial` finds it. It goes to serial and to the crash area, as
XCRL kind 7 at sector 520 (up to 8 KB, after the hardware report), so the
rig, which may have no serial port, keeps one. The save is a self-test of
its own, "profile: printed, saved to the crash area": the record is read
back and must equal the text printed (`tests/sabotage/1.9f-profile.sab`).

| Line | Fields |
|---|---|
| `profile` | CPUs; the TSC's rate (Hz); the stress test's window and the token passes it made (decimal) |
| `lock sched boot <cpu>` | takes, takes that found it held, spins, TSC ticks spent spinning, the longest hold (ticks): everything since that CPU started |
| `lock sched stress <cpu>` | the same in the stress test's window; the longest hold `-` (not kept per window) |

`cxk hw show <source> --profile` lists it, from a serial log or a disk
(an image, `diskN`: the rig's stick, read elevated); `cxk hw diff a b
--profile` sets two beside each other, either from either, every field a measure, so every difference is
expected and all of them are listed. Both add a note per lock and window:
the share of takes contended, and for the stress window the share of the
CPUs' time spent spinning (`HwDiff.Contention`). Under TCG those figures
are the emulator's, not the hardware's.

## 9. Next

- Step 9 (SMP) reads the MADT's CPU entries, with the rig's IDs above as
  its test case.
- The rig's profile: boot it, then `cxk hw show diskN --profile` and
  `cxk hw diff diskN <qemu serial log> --profile`.
