# The other CPUs
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: 1.9d done (2026-11-06): every CPU the MADT lists as enabled is
> started by the APIC ID it lists, sets up its own tables, per-CPU data and
> local APIC, and parks; none runs a thread yet** (`planning/CX_ROADMAP.md`,
> Phase 1 step 9). 1.9e puts the scheduler on them.

The rig is an FX-8350: eight cores, of which the kernel has used one. This
step starts the other seven and leaves them idle, so that 1.9e changes the
scheduler and nothing else. Code: `kernel/arch/smp.h` (the interface),
`kernel/arch/x86_64/smp.c` and `smp_tramp.asm`, `kernel/arch/x86/madt.c`
(which CPUs), `kernel/cpu/percpu.h` (each CPU's `struct cpu`).

## 1. Which CPUs: the MADT, by its IDs

A CPU is started if its MADT entry (a local APIC entry, type 0, or an
x2APIC entry, type 9) has the enabled bit set, and it is aimed at by the
APIC ID that entry names (`madt_cpus`). Two other sources are wrong on the
rig, which is why neither is used:

- **CPUID's initial APIC ID** (leaf 1, EBX[31:24]) says 0 for a boot CPU
  whose local APIC answers 0x10 (`hardware/CX_HW_REPORT.md` §7). An INIT
  aimed by it lands on nothing.
- **The number of entries** is 24 on the rig, of which 16 are disabled:
  firmware lists the CPUs a board could take. QEMU lists hot-pluggable
  CPUs the same way.

An entry naming an ID already kept (the same CPU as a local APIC and an
x2APIC entry) is counted once; an ID of 0xFF or more is not kept, as xAPIC
IPIs cannot name it; the online-capable bit alone is not enabled. Haiku's
loader reads the table the same way (`smp_do_acpi_config`,
`src/system/boot/platform/bios_ia32/smp.cpp`: the enabled flag, then
`apic_id`).

The verification boot (`BootVerifier.X86_64Smp`) gives QEMU
`-smp 4,maxcpus=6,sockets=2,cores=3,threads=1`: three cores take two bits
of ID, so the four CPUs are at 0, 1, 2 and 4, and the two possible ones
are listed disabled. A kernel that starts CPUs by count or by index, or
starts a disabled entry, fails every boot. `verify-boot --smp 1` boots one.

## 2. Starting one: INIT, STARTUP, park

One AP at a time, as Haiku's loader (`smp_boot_other_cpus`) and XNU
(`intel_startCPU` → `start_cpu`, `osfmk/i386/mp.c`) do. Serenity
broadcasts one INIT and two STARTUPs to all-but-self
(`APIC::do_boot_aps`, `Kernel/Arch/x86_64/Interrupts/APIC.cpp`), which
wakes whatever answers rather than what the MADT names, and needs a stack
per AP ready in the trampoline at once. One at a time lets one trampoline
page, and one stack word in it, serve every AP.

For each enabled entry but the boot CPU's own (`arch_smp_start`):

1. the next `struct cpu` (`cpu_next_slot`) and the AP's stacks, all from
   `kstack_alloc` (guarded): a 16 KiB idle stack, an 8 KiB RSP0 and four
   8 KiB IST stacks;
2. the trampoline's stack and `struct cpu` words written (`phys_map`);
3. INIT (asserted, then de-asserted for an older local APIC), 10 ms,
   STARTUP with the trampoline's page, 200 µs, and a second STARTUP if it
   has not answered: the Intel SDM's sequence (vol. 3A 8.4.4);
4. up to a second for it to park. One that never parks is sent INIT again,
   which stops it wherever it got to, before its stacks are freed, and is
   left out with a warning. Its slot goes to the next CPU.

The trampoline (`smp_tramp.asm`) is copied to physical 0x8000 (`SMP_TRAMP
_PHYS`), which must be usable RAM in the firmware's map; the loader's own
use of the first MiB ends before the kernel runs, and the page database
never hands it out. Real mode to protected mode on its own GDT, then the
boot CPU's CR4, a root built for it, EFER.LME and NXE (the kernel's tables
set the no-execute bit, reserved without NXE) and the boot CPU's CR0:
long mode. Real to protected first, as Serenity's `apic_ap_start` and
Haiku's trampoline do, rather than straight to long mode.

The root (`paging_boot_root`) holds the kernel half and the trampoline's
page at its own address, executable. It is not the kernel's root, whose
user half is empty by rule (`t_kernel_half`), and it comes from below
4 GiB, as the trampoline's `mov cr3` is 32-bit code. The page is mapped
borrowed, so `addr_space_release` frees the tables and never the page. It
is freed once every AP has started.

On the AP, `smp_ap_entry`: the kernel's root, then `arch_cpu_init_ap` in
`arch_cpu_init`'s order: its own GDT and TSS (`gdt64_init_cpu`: `ltr`
marks a TSS busy, so two CPUs cannot share one), its `struct cpu` behind
GS, its RSP0, the one IDT (a gate names an IST slot, which each CPU reads
from its own TSS, so nothing in the IDT is per CPU), the PAT (1.8d: every
AP must, or its write-combining pages are write-back) and the syscall
MSRs. Then its local APIC (`lapic_ap_init`): enabled, every priority
accepted, the spurious vector, and the timer divided by 16 on its vector
but stopped and masked. The feature table is the boot CPU's: the rig's
cores are one die.

## 3. Parked

A parked AP sits in `sti; hlt` with nothing routed to it: the I/O APIC
and every MSI name the boot CPU. It wakes for one thing, the wake IPI
(vector 57, `SMP_IPI_VECTOR`), which `smp_call` sends after leaving a
function in its mailbox (`struct cpu`'s `call`). The check of the mailbox
and the halt are one step: interrupts are off from the check to
`sti; hlt`, and `sti` holds them off for one instruction more, so an IPI
sent after the check ends the halt rather than being lost before it.

Any interrupt an AP takes goes through `irq_dispatch`'s first branch:
counted (`irqs`, and the timer's separately), acknowledged at its own local
APIC, and nothing else: no tick, no preemption, no sample for the random
generator. That branch is what 1.9e replaces.

A function run by `smp_call` runs with interrupts off and may not block,
take a lock or log. In 1.9d only the self-tests call it.

## 4. Tests

Host (`test_madt.c`): the rig's MADT shape, QEMU's gapped one, x2APIC
entries, duplicates, unreachable IDs, the cap and damaged tables.

Boot (`ktest_arch.c`), each reading the AP's state on the AP:

- every enabled MADT CPU runs, each started by its own listed ID, none
  twice, and the boot CPU's is its local APIC's;
- each AP has its own GDT, a busy TSS of its own, GS on its `struct cpu`,
  its own IST stacks and RSP0, the one IDT, `PAT_CXK`, the syscall MSRs,
  the kernel's root and the boot CPU's CR0 and CR4, and its local APIC
  enabled with the timer masked;
- an AP's own timer interrupts it twice (the second only if the first was
  acknowledged), counted on that CPU and never on the boot CPU;
- a parked AP halts: 50 ms with nothing asked wakes it never, a call wakes
  it exactly once by one interrupt; calls to the boot CPU, past the last
  CPU and of no function are refused.

## 5. Not done here

- Threads on the APs, IPIs between running CPUs, TLB shootdown, a reaper
  that checks a thread is off every CPU: 1.9e.
- The x2APIC: IDs past 0xFE are not started (none of the targets has one).
- A CPU that fails to start is tested by nothing but the code's reading:
  QEMU and Hyper-V start every CPU they list.
- The TSC's agreement between CPUs (XNU's `start_cpu` measures it): 1.9e,
  when time is read on more than one CPU.
