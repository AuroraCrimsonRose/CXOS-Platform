# The other CPUs
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: 1.9e2 done (2026-11-06): every CPU the MADT lists as enabled is
> started by the APIC ID it lists, sets up its own tables, per-CPU data and
> local APIC (1.9d); a changed mapping is shot down on every CPU that could
> hold it (§4, 1.9e1); and every CPU runs threads, from one scheduler, each
> with its own idle thread (§4a, 1.9e2)** (`planning/CX_ROADMAP.md`, Phase 1
> step 9); and the file system layer, logging and the CMOS clock are
> locked, the reaper runs on every CPU and the TSC agrees (§4b, 1.9e3).
> User memory is read and written only through recoverable copies
> (1.9e4a); 1.9e4b locks what interrupts still keep to one CPU, and then a
> process's threads leave the boot CPU.

The rig is an FX-8350: eight cores, of which the kernel used one until
1.9d started the other seven and left them idle, so that 1.9e changed the
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

## 3. Parked, then idle

An AP parks only until its idle thread first runs (1.9e2, §4a); from then
on its halt is that thread's, in `smp_idle_wait`, with nothing routed to
it: the I/O APIC and every MSI name the boot CPU. It wakes for the IPI on
vector 57 (`SMP_IPI_VECTOR`): a call left in its mailbox (`smp_call`,
`struct cpu`'s `call`), a shootdown, or a thread handed to it. The look
and the halt are one step: interrupts are off from the look to
`sti; hlt`, and `sti` holds them off for one instruction more, so an IPI
sent after the look ends the halt rather than being lost before it.

An interrupt an AP takes goes through `irq_dispatch`'s first branch:
counted (`irqs`, and the timer's separately), acknowledged at its own local
APIC, then its timer's slice check (`sched_cpu_tick`) and the preemption
point. No tick and no sample for the random generator: those stay the
boot CPU's.

A function run by `smp_call` runs in the AP's idle thread with interrupts
off, and may not block, take a lock or log. Only the self-tests call it.

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

## 4a. Threads on every CPU (1.9e2)

Every CPU runs threads, from one scheduler under the one `sched_lock`
(`cpu/sched.c`; the lock is CX_LOCK_ORDER.md §2a's).

**One order, not a queue per CPU.** `next_runnable` scans every thread for
the highest priority that may run on the calling CPU, round-robin within
a level, as before. A thread may run on a CPU if its `affinity` mask has
that CPU's bit, and it is either the one the CPU is running or ready and
on no CPU (`on_cpu`, the CPU's index plus one, 0 for none). The roadmap
said per-CPU run queues; this is Serenity's shape instead, one set of
ready queues behind one lock for every processor (`g_ready_queues`,
`Kernel/Tasks/Scheduler.cpp`), where Haiku keeps a run queue per CPU and
core (`scheduler_thread.h`, `ThreadData::Enqueue`) and WRK one per
processor (`KiSelectNextThread`, `base/ntos/ke/thredsup.c`). A scan under
the thread limit is short, and per-CPU queues need a balancer and
migration; they wait for a profile of the SMP stress suite (1.9f).

**An idle thread per CPU** (`sched_cpu_prepare`), WRK's `Prcb->IdleThread`
(`KiSelectNextThread` falls back to it). It is outside the id table and
the thread count (id `THREAD_IDLE_ID` plus the CPU's index), pinned to
its CPU, and is what the CPU runs when nothing else may. Its loop picks
under the lock, switches to what it found, or else gives the lock back
with interrupts still off and halts: the boot CPU by `timer_idle`, an AP
by `smp_idle_wait`, which first runs a call left in the mailbox and marks
the CPU idle for the shootdown (§4). Before 1.9e2 a thread with nothing
to switch to halted on its own stack; with a second CPU that stack could
be woken and run there while the first still stood on it. An idle thread
is never switched from inside an interrupt: an idle CPU's interrupt path
touches no thread record before its wake has flushed (§4).

**Handing a thread a CPU** (`kick`): a thread made ready or raised goes to
a CPU it may use that is idle, or else the one running the lowest thread
below it; none, and it waits its turn. That CPU's `need_resched` is set,
and if it is another CPU it gets the IPI (vector 57), once per ask: the
exchange that sets the flag says whether one is already on its way. This
is Haiku's `enqueue` (`scheduler.cpp`: `atomic_get_and_set` on
`invoke_scheduler`, then `smp_send_ici`), and Serenity's waking of an idle
processor (`smp_wake_n_idle_processors`, `Arch/x86_64/Processor.cpp`). The
IPI ends an idle CPU's halt, or reaches the busy CPU's preemption point
after the EOI. A thread switched out still ready is offered to the other
CPUs the same way, so one moved by `thread_set_affinity` reaches its new
CPU even if that one is halted.

**Where threads run.** A thread is made for the boot CPU, as before
(`THREAD_ON_BOOT_CPU`); `thread_create_on` makes one for a mask, and
`thread_set_affinity` changes it. Only the self-tests ask for an AP so far:
the file system layer and logging are locked since 1.9e3 (§4b), but a
syscall still reads user memory directly after checking it (§4b), so a
process's threads stay on the boot CPU until 1.9e4.

**The reaper** frees only a thread on no CPU, on every CPU since 1.9e3: a
free can end a process and close its handles through the file system
layer, which is locked now (§4b). The `on_cpu` check is also implied by the
handoff (an exited thread's CPU clears it in its last switch, before the
lock comes back), and is kept as the reaper's own rule.

**Time.** The event timer and every expiry stay the boot CPU's. A deadline
made on an AP (`timer_deadline_hint`) is forwarded: the earliest is kept
for the boot CPU, which gets an IPI and sets its timer for it
(`timer_hints_take`, in its vector-57 branch). An AP's time slice is its
own local timer (`timer_local_arm`), set when a thread is switched to and
stopped for its idle thread or with preemption off; under the PIT there
are no AP slices. Each CPU has its own slice end, `need_resched` and
switch-in-progress flag (`struct cpu`).

**Shootdowns against an interrupts-off body.** A syscall's body and a
driver's bounded wait run with interrupts off (1.6c); a shootdown sent by
an AP waited out such a wait up to its timeout. The bounded wait's spin
(`timer_timeout_expired`) and `timer_sleep`'s busy loop now serve
requests, as `spin_lock`'s spin already did.

**Reading a thread.** A thread finds its own record through `struct cpu`'s
`cur` (one GS load); another's only under the lock (§2a of the lock order:
the id table can grow, and free its old array, on another CPU).

## 4b. The locks of 1.9e3, and the TSC

**What 1.9c left one CPU's.** The file system layer (CXFS, `sysfile`'s
open files, graveyard and advisory locks) and logging (the console, its
serial tee, the system log's ring) were serialized by running only on the
boot CPU with interrupts off. Each now has a lock (`CX_LOCK_ORDER.md`):

- **The storage lock** (`disk.h`, `LR_STORAGE`, first in the order): every
  CXFS entry point, every `disk_read`/`write`/`flush`, the crash log's
  saves, and each file syscall's body. CPU-recursive (`sync/rspin.h`): CXFS
  calls CXFS. A spinlock, as everything under it busy-polls the disk: a
  syscall's body already runs with interrupts off (FMASK), so holding one
  over a transfer costs no more than 1.9c did.
- **`files_lock`** (`sysfile.c`, `LR_SYSFILE`): the graveyard, the lock
  list and its waiters, and unlink's open check with its marking, so a
  last close on another CPU cannot slip between them. A waiter blocks as
  `kobj_wait` does: prepared under the lock, then given back, then blocked,
  so no release between is lost.
- **The log lock** (`console.h`, `LR_LOG`, last): every console write and
  every line, its ring record with it, so no CPU's line cuts another's.
  CPU-recursive. `SYS_LOG_READ` copies out of a local copy and
  `console_write` copies in 256 bytes at a time, so no user copy runs
  under it. A line and its child lines are separate calls: another CPU's
  line may fall between them (a caller that minds holds `log_lock`).
- **The CMOS lock** (`rtc.c`, `LR_CMOS`): the index and data ports are one
  read in two steps.

The crash path breaks the two recursive locks (`panic_begin`): it runs
through them unlocked rather than wait for a CPU that may never give one
back.

**The reaper** runs on every CPU, in each idle thread and after each
switch (§4a).

**The TSC's agreement.** `timer_now_ns` reads the TSC wherever it runs,
against a base the boot CPU took. XNU (`osfmk/i386/mp.c`, `start_cpu`)
measures each AP at a two-sided barrier and panics or logs past
`TSC_sync_margin`; Haiku (`arch_cpu.cpp`, `arch_cpu_preboot_init_percpu`)
brings the CPUs to one rendezvous and writes 0 to each TSC. Here each AP,
as it parks, meets the boot CPU for 64 round trips: the boot CPU reads b0,
asks, the AP reads its TSC, the boot CPU reads b1; the AP's offset is its
reading less the midpoint, give or take half the shortest trip. Past that
and XNU's margin (4096 cycles) the offset goes into the AP's `tsc_adjust`,
which the clock takes off every reading there, and it is measured again.
Not a TSC write: QEMU's TCG ignored one in this step's test, and the
FX-8350 has no `IA32_TSC_ADJUST`. The boot log says how many APs were set
right.

**Why a process's threads still stay on the boot CPU.** A syscall checks a
user range (`user_ptr_readable` and the rest) and then reads it directly:
148 such reads, most in `usermode.c`. A sibling thread on another CPU can
unmap the range between the check and the read, and the kernel faults;
only `user_copy_bytes` recovers from a fault. 1.9e4a moved them to
recoverable copies (about 30 were syscalls'; the rest were the tests' own
setup, now `diag/ktest_umem.h`), and the build refuses `user_ptr` outside
`usercopy.c`. What still keeps the threads home is what interrupts-off
alone serializes: the process table's counts and stop list, the jobs, the
keyboard's buffer and waiter, the framebuffer against the console, a
background process's output, and the network stack (1.9e4b). Run with
every thread free to move, 1.9e3's boot failed 11 tests, most of them
tests that assume one CPU's ordering (yield loops): 1.9e4b's list.

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
- an idle AP halts: 50 ms with nothing asked wakes it never, a call wakes
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

The scheduler on every CPU (1.9e2). Each test that makes threads waits
for them to be reaped, ending every AP's halt with a call each 100 ms
until they are, so a failed test's thread left ready on a halted AP
cannot redden the tests after it:

- an AP's shootdown aimed at the boot CPU while it polls with interrupts
  off, in a driver's bounded wait (`timer_timeout_expired`), finishes and
  is served;
- each CPU has its idle thread: pinned, outside the id table, the count
  and `thread_set_affinity`; each AP runs its own, and the boot CPU runs
  the test's thread;
- a thread made as before runs on the boot CPU, even while the boot CPU
  is busy and the APs are idle; one made for each AP runs there; one for
  no CPU there is is refused;
- a thread blocked on an idle AP, woken from the boot CPU, runs within
  100 ms, by a resched IPI and a wake of that AP;
- a 20 ms sleep on an AP ends between 20 and 80 ms, while the boot CPU
  spins and sets its timer for nothing of its own;
- two threads on one AP that never yield both run, by its own timer's
  slice;
- a thread raised above the one a busy AP runs takes it within 100 ms,
  with a slice of a second;
- a running thread whose affinity moves from AP 1 to AP 2 runs on AP 2
  within 100 ms;
- a thread two CPUs may run, woken while still on its CPU, is not taken
  by the other (the one test of the `on_cpu` check, §4a);
- the getters on another thread take the scheduler's lock (ktest_hooks).

1.9e3 (`ktest_arch.c`, four CPUs):

- the storage lock, the log lock and `files_lock`, each held on the boot
  CPU, keep an AP's thread in the call that takes it for 30 ms, and let it
  through once given back; the storage lock is recursive and only its
  outer take ordered; the CMOS lock is taken by a wall-clock read;
- the system log's ring with two CPUs recording 200 each at once: every
  record whole and numbered next, each CPU's in its order;
- the APs reap: a thread ends on AP 1 and is freed while the boot CPU
  spins with interrupts off, which reaps nothing;
- every AP's TSC agrees with the boot CPU's, as bring-up left it and at a
  meeting now; an AP's put 10^10 cycles ahead, then half the count behind,
  is found out (the sign and at least half the size) and set right; its
  clock then reads between two of the boot CPU's, and with its offset moved
  by a second reads a second ahead.

## 6. Not done here

- A run queue per CPU, and a balancer (§4a): when a profile says the
  scan costs (1.9f).
- A process's threads on the APs: once proc, jobs, input, the
  framebuffer and the network stack are locked (1.9e4b, §4b).
- The futex reads its word with `futex_lock` held, so a fault on that
  page is demand-mapped under a spinlock (as the direct read before 1.9e4a
  was). Unverified whether that path can sleep; if it can, read under the
  lock without faulting and, on a fault, drop the lock, touch the page and
  retry (Linux's `futex_wait_setup`, from memory, not indexed). 1.9e4b's
  lock audit.
- `thread_proc`'s process outlives the lock it was read under: a thread's
  process is held by the thread, so it holds for any caller that knows
  the thread lives, which is every caller today; nothing checks it.
- An AP has no time slice under the PIT (no local timer is calibrated
  then): its threads run until they block or yield.
- Shootdowns are one page at a time: `vm_unmap` of a long region sends
  one per resident page to each CPU running the space. A range (Haiku
  collects a translation map's pages and flushes once, `Flush`) when
  ring 3 runs on several CPUs.
- Untested: the fence pairs (no test can make the ordering go wrong under
  TCG), `kstack_free`'s new order (a stale use after the free would fault
  on the AP inside a call), `icr_send`'s interrupts-off section, and the
  withdrawal after a timeout.
- The x2APIC: IDs past 0xFE are not started (none of the targets has one).
- A CPU that fails to start is tested by nothing but the code's reading:
  QEMU and Hyper-V start every CPU they list.
- The TSC measured once, at bring-up: a TSC that drifts later (not
  invariant, or a CPU's slowed apart) is not caught. A recheck when the
  rig shows one. An AP's shootdown timeout reads the clock, which agrees
  now.
- Untested: a TSC truly out at bring-up (QEMU's agree), so the bring-up
  path's correction is the test's meeting run again.
