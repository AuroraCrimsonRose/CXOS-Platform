# The kernel plan
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN, for x86-64.** What the kernel base, objects, processes,
> waiting, IPC and scheduling become. The kernel as built is described in
> `docs/x86_32/kernel/` until each part here lands. References are to the
> indexed source of the reference systems.

---

## 1. Where the inherited kernel stands

Kept, because it is already ahead of most hobby kernels: guard pages around
every kernel stack and a double fault on its own stack; typed boot
configuration (`kernel.xkco`); signed loading; self-tests each proven by
sabotage; advisory byte-range locks with deadlock detection.

What it lacks, and what this plan answers:

| Gap | Today | Section |
|---|---|---|
| No process object | Done in roadmap step 2a: `cpu/proc.h` holds the handle table, uid, cwd and address space; threads may share one. Since 2d the process frees its address space and handles when its last thread goes, however it went (a fault included) | §3 |
| Fixed small tables | Grow on demand since steps 2b and 2c: threads and pids to 256 (the kernel-stack slots bound them), handles to `max_handles` per process (16 to 4096, 1024 by default) | §3 |
| No way to learn a child's exit, or to kill | `sched.c` stores an exit code nothing returns | §4 |
| One thing waited on at a time | input, one endpoint, or sleep | §5 |
| Narrow IPC | Since roadmap step 1.3a, calls queue with reply tokens and badges, up to 64 KiB each way (ABI 20). Still no handle passing and no shared memory | §6 |
| No user threads, no futex, no pipes | | §7 |
| x86 code mixed with generic code | Step 3b moved the GDT, IDT, TSS, context switch, entry stub and linker script to `kernel/arch/x86_32/`, and the local APIC and port primitives to `kernel/arch/x86/`. Step 3c put paging behind `arch/paging.h`. Step 3d put every port access behind I/O windows (`drivers/iowin.h`) and moved the legacy PC devices to `kernel/platform/pc/`. Step 3e moved the last CPU-specific code out of generic files (the interrupt flag, halting, the cycle counter, the stack switch, the reset, the syscall gate, the register frame, MSI addresses, the self-tests that read the CPU's tables) and added the check: the kernel build refuses inline assembly, assembly sources, CPU intrinsics and `arch/<name>/` includes anywhere outside `kernel/arch/` (`tools/cmake/check_arch_boundary.cmake`). Step 3f put the PC's fixed devices behind `platform/platform.h` (the tick, the reset and poweroff lines, the wall clock, the boot text screen, the built-in keyboard and mouse; implemented in `platform/pc/platform.c`), moved the PS/2 self-test to `platform/pc/ktest_pc.c`, extended the check to refuse `platform/<name>/` includes outside `kernel/arch/` and `kernel/platform/`, and moved every memory-mapped driver onto windows: AHCI, e1000, RTL8169, OHCI, EHCI, xHCI (four windows, refused if one lies outside the mapped BAR), the local and I/O APICs, and the MSI-X table entry. **Done 2026-10-18.** | §2.2 |
| Boot data at fixed addresses | Read from `cxk_boot_info` since step 4a: stage 2 fills it and passes it in EBX, `kernel/boot/cxbi.c` checks and copies it, and the PMM, framebuffer and ACPI read nothing else. The loader poisons its old addresses and a self-test checks the poison. **Done 2026-10-18.** | `boot/CX_BOOT.md` §4 |
| Uniprocessor locking | critical sections switch interrupts off | §8 |
| First-fit linked-list heap | `heap.c` | §8 |
| PIT at 1000 Hz | | §8 |
| No stack protector | `-fno-stack-protector` | §8 |

---

## 2. Phase 0 foundations

### 2.1 One kernel object model

Every kernel object starts with the same header: type, reference count, the
rights a handle may carry, and a **signalled** state the wait queue reads.
Processes, threads, endpoints, files, pipes, memory objects, timers,
interrupts and fences are all objects. Lifetimes follow the reference count,
which retires the special cases that grew up without it (the file graveyard
is the current example).

NT's object manager (`WRK-v1.2/base/ntos/ob`) is the model. Linux shows the
alternative: many unrelated kinds of file descriptor.

**Grant chain.** Each handle records which handle it was derived from. That
is what lets revoking a grant revoke everything derived from it, and lets the
Process Manager answer "why can this do that?". It is a field now; adding it
later would touch every place a handle is created.

**One recording point.** Every syscall and IPC message passes through one
place. Nothing records yet; the rule keeps process record and replay possible
later.

**Safe across CPUs from the start.** Reference counts are atomic and every
handle table has its own lock, even while one CPU runs. SMP (§8) then turns on
without revisiting each object.

### 2.2 Architecture layer

`kernel/arch/<arch>/` holds the CPU-specific code (GDT, IDT, TSS, context
switch, entry stubs, paging); everything else is generic. Paging sits behind
one interface with one implementation per architecture, as Haiku does
(`src/system/kernel/arch/x86/paging/{32bit,pae,64bit}`). XNU's `osfmk/i386`
beside `osfmk/kern` and NT's HAL are the same idea. The 32-bit code moves
there first, as the baseline that still boots; it is deleted once x86-64
passes the full boot (roadmap Phase 0.6). ARM will be the next directory.

The layout (step 3b): `kernel/arch/x86_32/` and later `x86_64/` hold what
differs by width; `kernel/arch/x86/` holds what both share (the feature table,
the local APIC, port I/O). Generic code does not include either. It includes
the arch-neutral interfaces beside them, which every architecture implements:
`arch/cpu.h` (the processor's tables, the stack taken on entry from user mode),
`arch/irq.h` (handlers, MSI vectors and addresses, the interrupt controllers;
the register frame is opaque), `arch/paging.h` (step 3c), `arch/cpu_ops.h`
(the interrupt flag, halting, a spin hint and the cycle counter, inline
because they are single instructions), `arch/usermode.h` (entering and leaving
user mode, the syscall gate) and `arch/ktest_arch.h` (the self-tests that must
read the CPU's own tables) (step 3e). The kernel build enforces the
boundary: `tools/cmake/check_arch_boundary.cmake` runs before the link and
refuses inline assembly, assembly sources, CPU intrinsics and includes of
`arch/<name>/` in any file under `kernel/` outside `kernel/arch/` (and
`kernel/test/`, whose host tests test arch code directly), built or not. A mapping is asked for by what it allows,
`VM_WRITE`, `VM_EXEC`, `VM_UNCACHED`, never by page-table bits, and there is
no flag that makes a kernel mapping reachable from user mode: only
`paging_map_user` can, and only below `KERNEL_VBASE`. An address space is named
by its root table's physical address (`paddr_t`, 32 bits on x86-32). The 32-bit
recursive directory, its entry format and the propagation of new kernel-half
entries are private to `arch/x86_32/`; the encoders (`pagetable.c`) are
host-tested. `VM_EXEC` is accepted everywhere and enforced where the MMU can,
which on x86-32 without PAE is nowhere; x86-64 enforces it with NX.
`kpanic` is declared in `kernel/panic.h`. Device registers are reached
through an I/O window, port-mapped or memory-mapped, so a driver's logic is
the same for both (SerenityOS `Kernel/Library/IOWindow.cpp`); only the window's
port backend is x86 (step 3d). `drivers/iowin.h` opens a window over a range
of ports or of mapped device memory, and reads and writes it by offset. An
access not wholly inside the window is refused: a read gives all ones, a write
is dropped, and both are counted, so a self-test can tell that no driver
reached past its registers. The port instructions themselves are in
`arch/pio.h`, which only `iowin.c` includes; on an architecture with no port
space a port window simply fails to open. The legacy devices, whose ports
never move, use windows fixed at build time (`IOWIN_PORTS`). Legacy PC devices are platform code, `kernel/platform/pc/`, not arch code:
the 8259 PIC, the PIT, the 8042 (one file owning the controller, with the PS/2
keyboard and mouse drivers on it), the CMOS clock, the speaker, VGA text, and
the emulators' fixed poweroff ports. Generic code reaches them only through
`platform/platform.h` (the tick, reset, poweroff, wall clock, boot text
screen, and built-in input), which `platform/pc/platform.c` implements; the
build refuses a `platform/<name>/` include anywhere else outside
`kernel/arch/`. A device class the rest of the kernel
uses keeps a generic front: `drivers/input/keyboard.c` is a buffer of keys and
`mouse.c` a pointer position, fed alike by the PS/2 drivers and USB HID.

### 2.3 CPU features, detected at boot

CXOS is built for any x86-64 CPU, not for the rig. The kernel reads CPUID once
at boot into one feature table, and every optional path asks the table.

- **Required:** long mode, NX, APIC, and the SSE2 that every x86-64 CPU has.
  Without them the loader refuses with a clear message.
- **Used where present:** SMEP, SMAP, UMIP, PCID, 1 GiB pages, XSAVE and AVX,
  x2APIC, invariant TSC, RDRAND/RDSEED, CET (later).
- **Counted, not assumed:** CPUs from ACPI's MADT, memory from E820 or the UEFI
  map, devices from PCI and ACPI.
- The kernel itself is built without SIMD (`-mno-sse` and kin). Programs are
  built for baseline x86-64; a faster path (AVX for the compositor, say) is
  chosen at run time.
- **Tested both ways.** QEMU boots with a minimal CPU model and a full one, so
  both sides of each feature check run. Done for the 32-bit kernel in roadmap
  step 3a: `cxk verify-boot --cpu pentium2,-apic` (no local APIC, no SSE) and
  `--cpu max`. The table is `kernel/arch/x86/cpufeat.h`, printed at boot; its
  decoder is host-tested on register values from CPUs QEMU does not model.
  The 32-bit kernel needs an i686: it is compiled with `cmov`, so a plain
  `pentium` model panics at the first one, with no message saying why.

### 2.4 Memory layout

Four-level paging, 48-bit addresses.

| Range | Holds |
|---|---|
| `0x0000_0000_0000_0000`–`0x0000_7FFF_FFFF_FFFF` | the process (lower half); page 0 never mapped |
| `0xFFFF_8000_0000_0000` onward | every physical page, mapped once (the direct map), NX, large pages |
| a region above the direct map | kernel heap, kernel stacks with guard pages, device registers |
| `0xFFFF_FFFF_8000_0000` (top 2 GiB) | the kernel image, so clang's kernel code model applies |

The upper half is shared by every address space. KASLR later slides the image
and the direct map, seeded from `cxk_boot_info.rng_seed`.

### 2.5 x86-64 bring-up

Long-mode paging with NX on every non-code mapping; a 64-bit GDT, IDT and TSS,
with the double fault on an IST stack (the 32-bit task gate maps onto IST
directly); `syscall`/`sysret` as the transition; the self-test suite passing.
Kernel stacks keep their guard slots.

**Step 5b, done 2026-10-24.** `kernel/arch/x86_64/`: `entry.asm` moves to a
stack in the image; `gdt.c` installs ring-0 code and data and a 64-bit TSS
whose IST1 is the double-fault stack (the user segments wait for 5e, whose
`STAR` layout fixes their order); `idt.c` and `isr.asm` take the 32 CPU
exceptions, the double fault on IST1, anything unexpected a panic on COM1.
`boot64.c` runs eleven bring-up self-tests and halts: long mode and NX
active, the image and its stack in the top 2 GiB, CXBI handed over, the GDT,
TSS and IDT loaded, `#BP`, `#UD` and `#GP` delivered with the right RIP and
error code, and a double fault forced from a non-canonical RSP arriving on
its IST stack. Without the IST that fault is a triple fault, so the boot
stops with no summary line. Generic code joins in 5c.

**Step 5c, done 2026-10-24.** The kernel takes the handover through the
generic `boot/cxbi.c` while the loader's mapping of low memory still exists,
then `arch/x86_64/paging.c` replaces the loader's tables instead of adopting
them: a PML4 with all 256 kernel entries made at boot (so a new space copies
them and nothing is ever propagated), the direct map of every RAM entry of the
memory map at `DIRECT_MAP_BASE`, writable and no-execute, 2 MiB pages where a
whole one fits and none over a hole, and the image section by section (text
read-only, rodata read-only and no-execute, data and bss no-execute). The low
identity map goes with the loader's tables, so a null pointer faults; CR0.WP
is set. Until the switch the new tables are written through the loader's
window, so their pages come from below 1 GiB (`pmm_boot_alloc`); after it,
every table is reached through the direct map, in any space, which replaces
x86-32's recursive slot and temporary mapping. `paging_map_phys` maps at
`PHYS_WINDOW_BASE` plus the physical address, so a frame always comes back at
the same place. The kernel half's layout, x86-32's fixed windows included, is
named once per architecture in `paging_defs.h` (`KVA_*`) and generic code uses
the names. The page allocator is §9.3's: `memman/buddy.c` (pure, host-tested)
over a page database of 16 bytes a page, placed at the top of the highest
usable range, with the zone below 4 GB; `memman/pagedb.c` implements `pmm.h`
on it and panics on a double free. x86-32 keeps its bitmap until step 6,
because its 4 MiB boot window cannot hold a database. Sixteen more bring-up
self-tests (27 in all), and `cxk verify-boot --arch x86_64` boots with 5 GB so
both zones and the hole below 4 GB are always exercised.

**Step 5d, done 2026-10-24.** Interrupts, the APICs, the tick, the context
switch and the scheduler run on x86-64, as generic code: `cpu/sched.c`,
`cpu/proc.c`, the object caches, the heap, guarded kernel stacks,
`object/kobj.c` and `htab.c`, `drivers/char/timer.c`, the 8259 and the PIT,
`drivers/power/acpi.c` and `arch/x86/apic.c` (whose interrupt-off handover
now uses `arch_irq_save`, not a 32-bit `pushfl`). The architecture adds
`switch.asm` (rbx, rbp, r12-r15 and RFLAGS), `cpu.c` (`arch/cpu.h`: RSP0 in
the TSS, a new thread's first frame with IF set and RSP aligned as after a
call), and in `idt.c` and `isr.asm` the legacy IRQs at 32-47, the MSI pool at
48-55, a spurious-vector gate that only returns, and the dispatch in x86-32's
order: handler, EOI to whichever controller delivered it, then the deferred
preemption point. The tick stays the 1000 Hz PIT (Aurora, 2026-10-24); a
deadline-programmed LAPIC timer is a later change of its own (it came in
roadmap Phase 1.2c, with the TSC clock). `boot64.c` runs
kmain's order up to `irq_controller_init`, thread 0 on a guarded stack, and
twelve more self-tests (39 in all): the heap, the guard below thread 0's stack,
the tick, the I/O APIC routing it with the 8259 masked, 100 ticks against the
ACPI PM timer, sleep bounds, cooperative and preemptive switching, a switch
keeping the callee-saved registers, a new thread starting with interrupts on,
RSP0 following the running thread, and the MSI pool. `klog`, the tick's
source and `keyboard_tick` are small stand-ins in `arch/x86_64/bringup64.c`
until the console, the PC platform and the input drivers run here. kmain and
`ktest_run` take over from `boot64.c` with the drivers and ring 3 (steps 5e
and 5f), not at 5d as first planned (Aurora, 2026-10-24).

**Step 5e, done 2026-10-24.** Ring 3 on x86-64, through `syscall`/`sysret`.
The GDT gains ring-3 data and code after the kernel's, the order `STAR`
fixes (sysretq loads SS from `STAR[63:48]` + 8 and CS from + 16); the TSS
moves to `0x28`. `arch_syscall_gate_init` sets `EFER.SCE`, `STAR`, `LSTAR`
and `FMASK` (IF, TF, DF, AC, NT and IOPL cleared on entry).
`arch/x86_64/usermode.asm` implements `arch/usermode.h`: `enter_usermode`
enters by `iretq` with every register cleared, the syscall entry moves to
RSP0 (read from the TSS) in its first two instructions and keeps every
register but rax, rcx and r11, and `return_to_kernel` is x86-32's. The
calling convention until step 6: number in rax, arguments in rdi and rsi (32
bits each, as the ABI's are), result in rax. Three hazards of that entry are
closed: NMI, `#DB` and `#MC` get IST stacks of their own (IST 2-4), since one
landing before the stack switch would push its frame wherever ring 3 left
RSP; `paging_map_user` refuses the user half's last page, so the RIP sysretq
returns to is always canonical (Intel faults in ring 0 on the user's stack
otherwise; Linux's `TASK_SIZE_MAX` leaves the same page out); and the entry
reloads SS before sysret, for AMD's sysret, which keeps a null SS's unusable
attributes (Linux `X86_BUG_SYSRET_SS_ATTRS`; QEMU does not model it, so only
the rig can show it). The user copy's recoverable region and its page-fault
recovery are ported, and the pointer checks and copies moved out of
`usermode.c` into `cpu/usercopy.c`, which both kernels link. Until kmain
takes over (5f), `bringup64.c` stands in for the syscall table with
`SYS_EXIT`, `SYS_YIELD`, `SYS_GETPID` and `SYS_CONSOLE_WRITE`, through the
recording point. Fifteen more self-tests (54 in all), each a 64-bit blob in
a process of its own: CPL 3 with registers cleared, the code mapped R-X and
the stack RW- and no-execute, every register kept across a syscall,
`SYS_CONSOLE_WRITE` and a refused buffer counted at the gate, the dispatcher
and the recording point, a fault ending the process and not the kernel,
ring 3 unable to read kernel memory, execute its stack or write its code,
a user stack overflow faulting on its guard page, a kernel stack overflow
arriving as a double fault on IST 1, each layer of the user copy on its own,
the tick preempting two processes spinning in ring 3, and a process giving
back every page and heap block whether it exited or faulted.

**Step 5f1, done 2026-10-24.** kmain runs on x86-64. Its source list is
x86-32's minus `arch/x86_32/` and the bitmap `pmm.c`, plus the x86-64 files,
so a generic file cannot be left out of one kernel and not the other.
`arch_memory_init` states each architecture's order once (x86-64 builds its
tables before the page database; x86-32 the reverse), and `paging_init`
keeps the VGA text buffer where the loader had it. Every driver probes and
the first-boot install runs. `boot64.c` became `arch/x86_64/ktest_arch.c`,
whose 54 cases `ktest_run` runs first (they check boot's state: the kernel
space's user half empty, DS as boot set it), and `bringup64.c` is gone:
logging, the tick, the keyboard and the syscall table are the real ones. X
compiles to x86-32 only, so `CXK_X_USERLAND` is 0 on x86-64: no executive,
and `arch/x86_64/xdata64.c` refuses every X Data document, so `kconfig`
boots on its defaults (TRANSITIONAL_CODE.md). The port found five narrowings
that 4 GB of RAM had hidden: `ocache_unmap` cut the virtual address to 32
bits and freed nothing, the xHCI scratchpad took pages above 4 GB, the
process-exit test mapped a truncated frame, the stack test truncated its
own address, and `kstack_init` and `ocache_sys_init` made a page table every
4 MB, which is x86-32's reach and every other one of x86-64's
(`PAGE_TABLE_SPAN`). User code pages are remapped read-and-execute once they are
written (W^X), since x86-64 enforces NX. 121 self-tests (67 + 54).

**Step 5f2, done 2026-10-24.** Device DMA memory comes from the allocator's
zone below 4 GB through one function, `dma_alloc` (`memman/dma.c`), which
maps it into the kernel half, zeroes it and records it in a ledger. The e1000
and RTL8169 regions were fixed at `0x700000` and `0x730000`, kept safe on
x86-32 only by `pmm.c` reserving `0x500000-0x800000`; x86-64's page database
never had that reserve, so there the e1000 shared RAM the allocator also
gave out. Both NICs now allocate, and the reserve is gone. AHCI took its
command structures and bounce buffer from `pmm_alloc_pages`, which on
x86-64 prefers the zone above 4 GB, and would then refuse them as out of its
32-bit reach. `usb_dma_alloc` became `dma_alloc`, so EHCI, OHCI and
xHCI use it too. One more self-test (68 and 122): every ledger region is
held by the allocator, lies below 4 GB and overlaps no other, and the ring
or command-list base each NIC and AHCI port holds, read back from its
registers, falls inside a region of its own name. xHCI's scratchpad pages
stay on `pmm_alloc_pages_low` directly (one page at a time, never mapped).

**Step 5f3, done 2026-10-25.** The loader checks an image's `arch_target`
against the kernel's own, `ARCH_CXEX_TARGET` (`arch/cpu.h`: 1 x86-32, 2
x86-64), instead of a fixed x86-32. On x86-64 it had accepted x86-32 images,
so a signed x86-32 program spawned there would have been entered in long mode
with its code read as 64-bit instructions. The loader self-test builds its
images for the native arch and requires the other x86 width, and an id
nothing defines, to be refused. The rest of the coverage needed no port: the
ring-3 tests already run 64-bit blobs, the spawn test refuses on the
signature before the arch matters, and the file and lock tests are kernel
code. File access from ring 3 through the executive waits for step 6, when
the DevKit writes `CXEX_ARCH_X86_64` (nothing writes it today). Self-test
counts unchanged (68 and 122).

**Step 5f4, done 2026-10-26.** The x86-64 kernel boots on the rig (FX-8350,
8 GB, booted from a USB stick on EHCI): all 122 self-tests, then idle. Three
faults stood in the way, none of which QEMU's verification boot could show.
The EHCI, OHCI and xHCI DMA windows were x86-32 addresses (`0xE3000000` and
on), in x86-64's user half, so `paging_map_kernel` panicked on the first
controller; they are `KVA_EHCI_DMA`, `KVA_XHCI_DMA` and `KVA_OHCI_DMA` in each
arch's `paging_defs.h` now. `verify-boot` attaches no USB controller unless
asked, so x86-64's USB had never run: `--usb ehci|ohci|xhci` boots now pass.
x86-64's panic wrote to COM1 only, so on the rig that panic looked like a hang
and left nothing behind; it now takes x86-32's path (crash-log buffer, COM1,
then a red screen on the framebuffer or VGA text, the panic tone, and the
record on the CRASH partition), and `kpanic` names its caller. The first
record read back that way showed the third: `STAR[63:48]` was `0x10`.
Intel's sysret ORs RPL 3 into SS and AMD's does not, so ring 3 ran with SS
`0x18` and the first interrupt's `iretq` back to it took #GP(0x18). The base
is `0x13` (`USER_DATA_SEL - 8`), as Linux's carries RPL 3, and the MSR test
checks AMD's arithmetic, which fails in QEMU on the old base. The SS reload
before sysret (5e) now runs on AMD hardware too: the ring-3 preemption tests
that switch inside an interrupt from ring 3 pass on the rig.

---

## 3. Processes and threads

A **process** owns the address space, the handle table, capabilities, uid and
cwd. A **thread** owns registers, a kernel stack and scheduling state. Thread
and handle tables grow on demand up to limits in `kernel.xkco`.

## 4. Process handles, stop and kill, jobs

- `spawn` returns a **process handle** with rights: wait, stop, kill, inspect.
  Pids remain for display only.
- **No zombies.** The record is freed when the process has exited and its last
  handle is closed. Nobody reaps.
- **Exit reasons** say how it ended: exited (with code), stopped or killed (and
  by whom), crashed (vector and address), over budget.
- **Stop** asks the process to exit through an endpoint it registered, then
  forces it after a deadline. Its right is handed out freely.
- **Kill** is absolute: no request, no deadline, nothing the target can do. A
  thread in a syscall is ended at the next safe point in the kernel, and its
  locks, handles and memory go through the same cleanup as an exit. Its right
  is separate and harder to obtain. The supervisor and the executive never
  hand out kill rights on themselves.
- **Jobs** (NT job objects, nested as in Fuchsia) hold a group of processes
  under one memory and thread budget and can end them together. This answers
  the x86-32 open question about a child that spawns without end.

References: Haiku `_user_wait_for_child` (`team.cpp`), NT `NtTerminateProcess`
(`psdelete.c`).

## 5. The wait queue

NT's `NtWaitForMultipleObjects` (`obwait.c`) re-checks every handle on every
call and is capped at 64; `poll` has the same cost. CXOS uses a **queue
object** instead (Fuchsia ports, kqueue, IOCP): register a handle with a key
once, and a wait returns only the keys that became ready. Registrations are
one-shot and re-armed, which avoids epoll's missed-wakeup and stampede
problems. Timers are handles with absolute deadlines on the monotonic clock. A
single-handle wait with a deadline covers the simple case.

## 6. IPC

Synchronous call and reply stays the fast path (L4, QNX). Added:

- **Several calls in flight.** Callers queue; the server receives each call
  with a one-shot **reply token** (seL4 reply objects, Mach send-once rights)
  and may answer in any order. A dead caller invalidates the token; a dead
  server fails its callers.
- **Badges.** Each send handle carries a value chosen by its creator, so a
  server knows who called without trusting the message or a reused pid.
- **Handles in messages**, moved by default, copied only with rights that can
  only shrink (XNU `ipc_kmsg_copyin_port_descriptor`).
- **Memory objects** for shared memory (NT sections, Fuchsia VMOs), with
  **sealing**: a sender can freeze a buffer so the receiver can use it without
  it changing after the check.
- **Notifications:** one-bit signals that combine, delivered through the wait
  queue (seL4).
- **Priority passed through calls:** a server runs a call at the caller's
  priority (seL4 MCS, QNX), so the broker cannot become a priority-inversion
  bottleneck.

## 7. Pipes, threads, futex

- **Pipes** are a pair of handles over a bounded ring. Writing with no reader
  returns an error, not a signal. Standard input, output and error are handles
  0–2 that `spawn` is told to pass.
- **User threads** with thread handles (join is a wait). **Futex** keyed by
  memory object and offset, so it works across shared memory; the futex word
  holds the owner's thread id by convention, which keeps priority inheritance
  possible later. **Thread-local storage** through `FS_BASE`. Guarded user
  stacks. Thread names and CPU time for the Process Manager.

## 8. The kernel base

| Area | Change |
|---|---|
| Locking | Spinlocks that save interrupt state; per-CPU data; a lock-order checker in debug builds (as Linux lockdep) |
| Allocator | Object caches by type and size class, per-CPU magazines, a page database and buddy allocator (§9) |
| Time | LAPIC timer and TSC; program the next deadline instead of a fixed tick; HPET as fallback |
| Hardening | Stack protector; KASLR seeded from `cxk_boot_info.rng_seed`; SMEP and SMAP where CPUID reports them; kernel UBSan in debug builds (Serenity `Kernel/Prekernel/UBSanitizer.cpp`); control-flow integrity later |
| Diagnostics | A crash log written at panic to a reserved disk area by a minimal polled write (no interrupts, no allocation, no filesystem), copied into a file on the next boot; RAM alone is not trusted, because a BIOS may clear or test memory during startup (**built for IDE and SATA in roadmap step 4b, USB in 4c**: `kernel/diag/`, the XCRL record in a CRASH partition, `cxk verify-crashlog [--usb]`; a panic that interrupts a USB transaction resets the stick and the controller's endpoints before the save, `disk_panic_reset`. An IDE or SATA transfer interrupted on the same disk is not yet recovered from); symbolised backtraces; a GDB stub over serial; a timestamped trace buffer; per-thread CPU time |
| Scheduler | Priority classes (real-time for input and audio, normal, background); per-CPU run queues; HLT when idle |

## 9. Memory

Decided 2026-10-16 after comparing `kernel/memman/` with Haiku, Serenity,
Redox, XNU and NT.

### 9.1 Where it stands

| Part | Today | Problem |
|---|---|---|
| Kernel heap (`heap.c`) | One first-fit linked list for every size | Allocating and freeing both walk the list; memory is never returned; each header sits directly before the data, so a small overflow corrupts the allocator |
| Physical pages (`pmm.c`) | One bitmap, searched from page 0 | Up to 65,536 words per search at 8 GB; `pmm_free` checks nothing, so a double free corrupts the count; contiguous runs by linear search |
| Process mappings (`vmregion.c`) | 32 regions per process in a fixed array | Every page allocated and zeroed when mapped; no shared memory |
| Paging (`paging.c`) | Recursive mapping, one temporary-mapping slot | The single slot cannot serve several CPUs; new kernel page directory entries need a hook to reach live spaces |

### 9.2 Kernel heap: object caches

- **One cache per kernel object type**, and size-class caches behind
  `kmalloc`. Objects of different types never share a slab, so a
  use-after-free can only be reused as the same type (XNU `kalloc_type`,
  `osfmk/kern/kalloc.c`).
- **Per-CPU magazines** in front of each cache, so most allocations take no
  lock (Haiku `slab/ObjectCache.cpp`, `ObjectDepot.cpp`; Bonwick's design).
  Serenity's `KmallocSlabheap` (`Kernel/Heap/kmalloc.cpp`) is the simpler
  version; NT's lookaside lists are the same idea.
- Allocations over a page or two come straight from the page allocator.
  Allocator records are kept away from the data. Empty slabs go back.
- Debug builds poison freed memory, add guard zones and catch double frees.

### 9.3 Physical pages

- **A page database:** one record per physical page with its state (free,
  zeroed, in use, page table, kernel) and a reference count (NT's PFN database,
  `base/ntos/mm/pfnlist.c`; Haiku `vm_page`; Serenity `PhysicalRAMPage`).
  About 16–32 bytes per page. Freeing a page that is already free is a panic.
- **A buddy allocator** for contiguous runs (Serenity
  `Kernel/Memory/PhysicalZone.cpp`, Redox `rmm/src/allocator/frame/buddy.rs`).
- **Zones:** a pool below 4 GB for devices that address only 32 bits (the rig's
  OHCI USB controller is one). Asking for it is part of every DMA allocation
  (`dma_alloc`, roadmap step 5f2).
- **Pages zeroed ahead of time** by a low-priority thread, kept on a zeroed
  list (NT `MiZeroPageThread`; Haiku `page_scrubber`, `vm/vm_page.cpp`).
- **Per-CPU page caches** once SMP is on.

### 9.4 Process memory

- **Memory objects** hold pages; a mapping is a view of one. Pages arrive on
  the first touch, through the page-fault handler (Serenity
  `AnonymousVMObject`, Haiku `VMCache`). The same objects are the shared memory
  of IPC (§6).
- **Regions in a balanced tree**, not a fixed array (Haiku
  `VMUserAddressSpace`, NT's VAD tree, Serenity's `RegionTree`).
- **Strict commit.** Mapping memory reserves its pages up front; running out
  is a clean error from the call that asked. There is no overcommit and no OOM
  killer (NT and Serenity; Haiku's `vm_page_reserve_pages` does the same inside
  the kernel). Commit is charged to the process's job budget (§4), which
  replaces `vm_charge`.
- **Copy-on-write waits.** CXOS has no `fork`, where other systems need it
  most; it arrives with snapshots and file mappings.

### 9.5 Paging on x86-64

- **The direct map** (§2.4) replaces the temporary-mapping slot and the
  recursive mapping (Redox, Haiku x86-64).
- **All 256 kernel top-level entries are allocated at boot**, so every address
  space shares the kernel half and the propagation hook is deleted.
- Where CPUID reports them: PCID, so a process switch keeps the TLB; global
  pages for the kernel; large pages for the direct map and the kernel image.
  TLB shootdowns are batched once SMP is on.

### 9.6 Order

| When | Work |
|---|---|
| Phase 0.1, with the object model | Object caches by type; the new `kmalloc` |
| Phase 0.5, with the 64-bit kernel | Page database, buddy allocator with the below-4 GB zone, direct map, kernel top level at boot (**done in step 5c** for x86-64; PCID, global pages and 1 GiB pages wait) |
| Phase 1, with IPC and SMP | Memory objects, pages on first touch, the region tree, strict commit, background zeroing, per-CPU caches |
| Later | Swapping to disk, memory compression (XNU), NUMA |
