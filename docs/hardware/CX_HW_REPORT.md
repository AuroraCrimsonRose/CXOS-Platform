# The hardware report
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: 1.8a done (2026-11-04): the kernel builds the report, prints it
> to serial and saves it to the crash area. 1.8b done (2026-11-04): `cxk hw
> show`, `hw diff` and `rig hwreport` (§5). 1.8c (Hyper-V's clock read
> through it) is next** (`planning/CX_ROADMAP.md`, Phase 1 step 8).

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
| `clock` | the clock source; whether the TSC is invariant; the TSC's rate measured against the tick and against the ACPI PM timer; the APIC timer's rate; the PM timer's port and width | `drivers/char/timer.c`, then a 50 ms measurement |

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
  and its stick is the only place a report can be carried off on.

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
  the TSC by the tick and by the PM timer agree within 1%.
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

## 6. Next

- **1.8c:** Hyper-V's TSC calibrates to 59742 MHz (HANDOFF): the report's
  two TSC rates say which clock is wrong there.
