# The other CPUs
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: 1.9e1 done (2026-11-06): every CPU the MADT lists as enabled is
> started by the APIC ID it lists, sets up its own tables, per-CPU data and
> local APIC, and parks (1.9d); a changed mapping is shot down on every CPU
> that could hold it (§4, 1.9e1). None runs a thread yet**
> (`planning/CX_ROADMAP.md`, Phase 1 step 9). 1.9e2 puts the scheduler on
> them.

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

## 4. TLB shootdown (1.9e1)

A CPU caches translations; once a second CPU runs, a mapping changed on
one is still used by the other until that one drops it. A frame freed
while another CPU still maps it is written by that CPU after its new owner
has it. So every change to a present entry goes through one place,
`entry_changed` in `arch/x86_64/paging.c`, which `paging_map_kernel`,
`paging_map_user_in` and `paging_unmap_in` all call. This CPU flushes the
page; then `smp_tlb_shootdown` (`arch/smp.h`) has every other CPU that
could hold it flush it too, and returns only once each has.
`paging_unmap_in` now returns the frame it unmapped, so the caller frees
it after that point. Three callers freed first (`kstack_free`,
`kstack_alloc`'s rollback, `fb.c`'s `copy_init` rollback), and all three
now free after. An entry that was not present needs nothing, since the TLB
caches no translation of a page that is not present (Intel SDM vol. 3A
4.10.2.3). No kernel page is global (`pagetable.c` sets no G bit), so a
CR3 reload drops everything, the kernel half included.

**Whom to ask.** Each CPU records the root its CR3 holds (`struct cpu`'s
`space`), stored before the load with a fence in between
(`addr_space_switch_on`). A change in the kernel half, which every space
shares, goes to every CPU; a change in a user space goes only to the CPUs
whose `space` is that root, as XNU's `pmap_flush_tlbs` reads each CPU's
`cpu_active_cr3` (`osfmk/x86_64/pmap.c`). Serenity broadcasts a user
address to every CPU when the process has more than one thread
(`ProcessorBase::flush_tlb`, `Kernel/Arch/x86_64/Processor.cpp`); Haiku
and WRK keep a set per address space of the CPUs it is active on
(`X86VMTranslationMap::Flush`'s `active_on_cpus`; WRK `KeFlushMultipleTb`'s
`Process->ActiveProcessors`, `ke/amd64/flushtb.c`). Reading CR3 per CPU
needs no set to keep up to date across switches. The sender writes the
tables, fences, then reads `space`; the switcher writes `space`, fences,
then loads CR3. Of two such sequences one always sees the other's write,
so a CPU entering the space either is asked or walks the new tables.

**Idle CPUs are not interrupted.** A CPU halted with nothing to run sets
`idle`; a sender marks it `tlb_stale` instead of interrupting it, and it
flushes everything as it wakes (`tlb_wake`), before it touches anything
that may have moved. This is XNU's rule for idle CPUs, "they'll check as
they go busy". The same store, fence, read pairing on `idle` and
`tlb_stale` means a request is either signalled or caught at the wake.
Until the wake, an idle CPU's interrupt path touches only what is never
unmapped: the kernel image, its own stacks and its `struct cpu`. On the
rig, seven idle cores are not woken every time a thread's stack is freed.

**Requests.** Each sender has one request, in its own `struct cpu`
(`tlb_out`: root, address, pages, and a bit per CPU still to answer), and
sets its own bit in each target's `tlb_from`. These are per-processor
packets, as WRK's `KiIpiSendRequest` uses, rather than one lock every
shootdown queues on. It then sends the IPI, vector 57, the vector the
park's wake already used, now served on every CPU (the boot CPU's has its
own branch in `irq_dispatch`, ahead of the MSI pool's). A target serves
every sender whose bit it finds (`smp_serve`). A request for a space the
target has since left is not flushed, since the CR3 load did that.
Past `SMP_TLB_FULL_AT` (32) pages it is a CR3 reload, as Haiku switches to
a global invalidate past `PAGE_INVALIDATE_CACHE_SIZE`.

**No deadlock.** Every spinlock turns interrupts off, so a CPU spinning
on a lock cannot take the IPI. If the sender holds that lock, it waits for
an answer that never comes. So `spin_lock` serves waiting requests between
looks at the lock word, as Haiku's `acquire_spinlock` processes pending
ICIs (`src/system/kernel/smp.cpp`), and the sender serves its own while it
waits, as Serenity's `smp_broadcast_wait_sync` does. Two CPUs shooting at
each other, or at a CPU spinning on the sender's lock, therefore both
finish. Serving takes no lock and only flushes, which is always safe. The
ICR write is now an interrupts-off section (`icr_send`): an IPI sent by a
handler between its two writes would have aimed the first at the wrong
CPU.

**A CPU that never answers.** `smp_tlb_shootdown` returns the CPUs that did
not answer within its timeout, and withdraws its bits from them. The
paging layer panics on any: carrying on would leave a CPU writing through
a mapping its owner believes gone. The timeout is 10 s, because a
syscall's body runs with interrupts off (1.6c) and serves only when it
spins. XNU panics the same way after `LockTimeOut`.

## 5. Tests

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

The shootdown (1.9e1), each changing a mapping an AP has just used, then
reading it again on the AP; two frames told apart by their first word
show a stale entry and a fresh one:

- a kernel page a busy AP is reading, first rewritten by hand with no
  shootdown, which the AP goes on missing (the control: the TLB under test
  does keep a stale entry, so the rest means something), then by
  `paging_map_kernel`, after which the AP reads the new frame and has
  served one request;
- an idle AP is not interrupted for a change (no interrupt, nothing
  served, marked stale), and its next wake flushes before it reads: the
  new frame, one stale wake, one interrupt;
- a user space's frame replaced while AP 1 runs in it and AP 2 in the
  kernel's: AP 1 reads the new frame and served it, AP 2 was never asked,
  and AP 1's recorded space follows its CR3 there and back;
- no deadlock: APs 1 and 2 shooting at each other at once, both waiting
  with interrupts off (each turns them off and waits for the other before
  it shoots; without that barrier one woke from its halt after the other
  had finished, and the first matrix found nothing crossed), both finish, and the boot CPU answers each through
  its IPI; and AP 1's shootdown aimed at the boot CPU while it spins,
  interrupts off from before the call, on a lock only that AP frees once
  its shootdown ends, finishes.

## 6. Not done here

- Threads on the APs: per-CPU run queues, an idle thread per CPU, the
  IPI that ends another CPU's halt for a wake, a reaper that checks a
  thread is off every CPU: 1.9e2.
- Shootdowns are one page at a time: `vm_unmap` of a long region sends
  one per resident page to each CPU running the space. A range (Haiku
  collects a translation map's pages and flushes once, `Flush`) when
  ring 3 runs on several CPUs.
- A shootdown sent by an AP waits on a CPU whose syscall body runs with
  interrupts off, up to the panic's 10 s. A disk read's busy-poll under
  `-icount` can last longer (1.4c). This is 1.9e2's to settle, since only
  then does an AP change mappings outside the self-tests.
- Untested: the fence pairs (no test can make the ordering go wrong under
  TCG), `kstack_free`'s new order (a stale use after the free would fault
  on the AP inside a call), `icr_send`'s interrupts-off section, and the
  withdrawal after a timeout.
- The x2APIC: IDs past 0xFE are not started (none of the targets has one).
- A CPU that fails to start is tested by nothing but the code's reading:
  QEMU and Hyper-V start every CPU they list.
- The TSC's agreement between CPUs (XNU's `start_cpu` measures it): 1.9e3,
  when time is read on more than one CPU. An AP's shootdown timeout reads
  its own TSC, relative to itself, so it holds without the agreement.
