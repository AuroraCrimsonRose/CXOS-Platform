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
| Narrow IPC | one call in flight per endpoint, one page copied, no handle passing, no shared memory | §6 |
| No user threads, no futex, no pipes | | §7 |
| x86 code mixed with generic code | Step 3b moved the GDT, IDT, TSS, context switch, entry stub and linker script to `kernel/arch/x86_32/`, and the local APIC and port primitives to `kernel/arch/x86/`. Step 3c put paging behind `arch/paging.h`. Step 3d put every port access behind I/O windows (`drivers/iowin.h`). Left: the legacy PC devices are still under `drivers/` (they go to `platform/pc/`), the memory-mapped drivers (AHCI, e1000, RTL8169, the USB hosts, the I/O APIC) still use raw pointers rather than windows, inline assembly in eight generic files (`sched.c`, `spinlock.h`, `kmain.c`, `ktest.c`, `power.c`, `acpi.c`, `timer.c`, `kmath.c`) and the unbuilt `cpu/fpu.c`, `usermode.c`'s view of the register frame, MSI addresses in `pci.c` | §2.2 |
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
`arch/irq.h` (handlers, MSI vectors; the register frame is opaque) and
`arch/paging.h` (step 3c). A mapping is asked for by what it allows,
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
never move, use windows fixed at build time (`IOWIN_PORTS`). Legacy PC devices (PIC, PIT, PS/2, CMOS clock,
speaker) are platform code, `kernel/platform/pc/`, not arch code.

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
| Diagnostics | A crash log written at panic to a reserved disk area by a minimal polled write (no interrupts, no allocation, no filesystem), copied into a file on the next boot; RAM alone is not trusted, because a BIOS may clear or test memory during startup; symbolised backtraces; a GDB stub over serial; a timestamped trace buffer; per-thread CPU time |
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
  OHCI USB controller is one). Asking for it is part of every DMA allocation.
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
| Phase 0.5, with the 64-bit kernel | Page database, buddy allocator with the below-4 GB zone, direct map, kernel top level at boot |
| Phase 1, with IPC and SMP | Memory objects, pages on first touch, the region tree, strict commit, background zeroing, per-CPU caches |
| Later | Swapping to disk, memory compression (XNU), NUMA |
