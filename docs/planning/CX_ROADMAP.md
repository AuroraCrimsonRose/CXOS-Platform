# CXOS Roadmap (x86-64 line)
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DIRECTION, not schedule.** Started 2026-10-16 on `x86_64-DEV`, from
> a series of design reviews that compared CXOS against SerenityOS, Haiku,
> Redox, XNU and NT (WRK and NT5), using the indexed source of each. It
> replaces the x86-32 roadmap (`docs/x86_32/planning/CX_ROADMAP.md`), whose
> reasoning still stands where this document does not say otherwise: the
> Build / Port / Embed / Skip rule, the languages, X Data as the one store, the
> AI and telemetry positions. Those sections are ported here when they are next
> touched, not before.

The detail lives in the design documents. This one holds the principles, the
order, and every decision with its reason.

---

## 1. Decisions (2026-10-16)

| Decision | Reason |
|---|---|
| **x86-64 is the primary target, and comes first.** | The test rig is a 64-bit CPU with 8 GB (`hardware/CX_TARGET_HARDWARE.md`). Everything built for 32-bit before the move gets ported afterwards, so the move goes ahead of the IPC, display and driver work. |
| **PAE is not a separate step.** | Long mode includes NX and the four-level page tables that PAE was a stepping stone to. |
| **ARM is deferred until after the ecosystem phase.** | Nothing on the near path needs it, and the rig is x86. |
| **x86-32 is retired.** The `x86_32_DEV` and `x86_32_RELEASE` branches stay as the reference. On this branch the 32-bit kernel and code generator are the working baseline only until their 64-bit replacements pass the full boot (Phase 0.6), then they are deleted. | The rig is 64-bit and ARM is far off; two x86 targets would double every kernel test for no user. A 32-bit port later starts from the reference branches. |
| **Built for any x86-64 CPU; features are detected at boot.** | The rig is the first test machine, not the target. Nothing is compiled for Piledriver: the kernel reads CPUID once at boot and every optional feature is used only where it exists (`kernel/CX_KERNEL_PLAN.md` §2.3). |
| **The kernel lives in the top half of the address space, with all physical memory mapped there.** | What every modern 64-bit kernel does; with 8 GB nothing needs mapping on demand (`kernel/CX_KERNEL_PLAN.md` §2.4). |
| **Kernel objects are safe across CPUs from the start.** | The object model is used everywhere; atomic reference counts and locked tables now mean SMP does not revisit every object later. |
| **A crash log that survives a reboot comes before the rig.** | A boot that fails on real hardware before the screen works shows nothing, and the rig may have no serial port to read it from. |
| **The assembler and linker in X (stage 5) leave Phase 0.** | Nothing in the 64-bit move needs them; ld.lld already links x86-64. |
| **Syscalls are one operation each on a handle; everything else is a typed service; one `.xfil` definition generates every binding.** | Fuchsia's shape. Multiplexed `*_OP` calls hide what a program does from tracing and recording, and three hand-kept copies of the ABI drift (`kernel/CX_SYSCALLS_IPC.md`). |
| **The network stack is a user-space service; foreign filesystems run in user space; CXFS stays in the kernel.** | Packets from the network and disks from outside are hostile input, the classic way into a kernel. The system disk is trusted and its speed matters most (`system/CX_NETWORK.md`, `system/CX_FILESYSTEM.md` §6). |
| **File names are case-insensitive and case-preserving.** | As on Windows and macOS; compared after Unicode normalisation and case folding (`system/CX_FILESYSTEM.md` §8). |
| **Interfaces are written in X Format Interface Language, `.xfil`.** | One definition for syscalls and services, named by the `x` + domain + type formula (`kernel/CX_SYSCALLS_IPC.md` §9). |
| **Only the boot disk's CXFS volumes mount in the kernel.** | Any other CXFS volume is treated as foreign and mounts in user space; `/System` is verified before it is parsed; the VFS lets CXFS move out later without a rewrite (`system/CX_FILESYSTEM.md` §9). |
| **Security is planned as one document, with the rig's gaps stated** (`security/CX_SECURITY.md`). | The rig has no verified boot, IOMMU, TPM, SMEP or SMAP; CXOS says so instead of implying protection it lacks. |
| **Correctness is established on a ladder: tests, property tests, fuzzing, TLA+ model checking, CBMC bounded proofs, and full proof only where it pays** (`safety/CX_SAFETY.md`). | Whole-kernel proof (seL4) costs about 20 person-years; proving the small core, model-checking the protocols and fuzzing every parser is what AWS and Microsoft do. Signature checking moves to formally verified crypto (HACL\*). |
| **X has no undefined behaviour outside `unsafe`, linear handles, and contracts** (`language/CX_X_PLAN.md` §5). | Every operation either has a defined result or traps; what cannot be checked is marked and searchable. |
| **`cxk` is never stale, quiet by default, and runs sabotage itself** (`devkit/CX_CXK_PLAN.md`). | The prebuilt has been found days out of date; builds print 300 lines for four that matter; sabotage by hand has destroyed real fixes. |
| **CXEX Studio (the Avalonia GUI) is retired.** | Editor support is the VS Code extension, with an X language server planned; everything else is `cxk`. Removed with it: `CXEX.UI` and the empty `CXEX.SDK` app. The solution is now `devkit/CXEX.slnx`. |
| **CXOS uses published fonts, never its own; text uses standard character sets.** | The console is Terminus (OFL-1.1) for IBM code page 437, converted from its BDF source at build time; the drawn `cx_term` fonts are removed. The GUI will use Noto (OFL-1.1) and Unicode (`system/CX_FONTS.md`). |
| **Memory: object caches by type, a page database with a buddy allocator, memory objects filled on first touch, strict commit.** | The heap, bitmap and eager mappings do not scale to 8 GB and 8 cores. Strict commit means running out is an error at the call that asked, never an OOM killer. Swapping, compression and NUMA are later (`kernel/CX_KERNEL_PLAN.md` §9). |
| **BIOS boot is the main path; UEFI is second.** | The rig has BIOS only. The BIOS loader must start a 64-bit kernel itself. |
| **GUI first, never GUI only.** | Every feature is a service with a typed IPC interface; the GUI and the CLI are both clients of it. The CLI remains the recovery and test path. |
| **Stop and kill are separate operations with separate rights.** | Stop asks politely and forces after a deadline; it is the everyday one. Kill is absolute, immediate and unrefusable, harder to obtain, and confirmed in the UI. |
| **The task manager is called the Process Manager.** | Clear, and it is what people search for. Revisit only if its scope outgrows processes. |
| **The display server draws window frames.** | Consistent frames, and a frozen app can still be moved and closed. |
| **The compositor runs retained layers and animations.** | Smooth animation even when an app is busy; the largest visible difference from the others. |
| **The first native GPU is AMD.** | The rig's GPU is a FirePro W5000. |
| **Novel features are kept only where they solve a real problem** (§5). | Several first ideas were novelty for its own sake; the trimmed list is what survives. |

---

## 2. Principles

Four rules run through every design document.

1. **Authority only through handles.** Nothing is done to a process, file,
   pipe or memory object except through a handle you were given. No syscall
   takes a pid or a global name as authority. The `GRANT_*` bits remain as a
   coarse outer limit.
2. **One way to wait for everything.** Process exit, thread exit, pipes, IPC,
   notifications, timers, interrupts and GPU fences are all waitable handles,
   delivered through one wait queue.
3. **No signals.** Notifications go to the wait queue; faults go to an IPC
   endpoint as messages. Nothing interrupts code at an arbitrary point.
4. **Nothing is inherited unless named.** A child gets exactly the handles its
   `spawn` lists.

Carried over from x86-32, unchanged: **do the cheap things that are painful to
retrofit; defer the expensive things that are not**, and every check is proven
by breaking it on purpose.

---

## 3. Phases

### Phase 0: the 64-bit foundation *(now)*

Done in order; each step boots with all self-tests passing before the next.

Steps 1–3 land on the inherited kernel, which still boots and so checks them.
The kernel is C, so it boots on x86-64 (step 5) before the X compiler can build
programs for it (step 6).

1. **Kernel object model.** One header for every kernel object: type,
   reference count, rights, signalled state, safe across CPUs. Each handle
   records the grant it came from (§5). Every syscall and IPC message passes
   one recording point. Object caches by type replace the linked-list heap
   (§9.2). See `kernel/CX_KERNEL_PLAN.md` §2.1.
2. **Processes separate from threads.** Handle table, capabilities, uid, cwd
   and address space move from the thread to a process object. Thread and
   handle tables become dynamic.
3. **Architecture layer and CPU features.** CPU-specific code moves under
   `kernel/arch/`, beneath generic code; paging behind one interface (Haiku's
   model). CPUID is read once at boot into one feature table (`kernel/CX_KERNEL_PLAN.md` §2.2–§2.3).
4. **Boot hands over through CXBI, and the crash log.** The kernel reads
   `cxk_boot_info` instead of fixed addresses. A panic writes a crash log to a
   reserved disk area through a minimal polled write, because a BIOS may clear
   RAM during startup; the next boot saves it (`kernel/CX_KERNEL_PLAN.md` §8).
   See `boot/CX_BOOT.md`. The CXBI half landed 2026-10-18 (4a). The crash log
   for IDE and SATA disks landed the same day (4b): a CRASH partition in every
   image, the panic's screen and the system log's tail written there by the
   drivers' polled transfers, `/System/crash.log` on the next boot, and
   `cxk verify-crashlog` proving it across two boots in QEMU. The USB stick
   the rig boots from followed on 2026-10-21 (4c): a panic that lands in the
   middle of a USB transaction now resets the stick and its controller's
   endpoints before the save (Bulk-Only reset recovery; real Stop/Reset
   Endpoint on xHCI), and `cxk verify-crashlog --usb xhci|ehci|ohci` proves it
   in QEMU with a READ abandoned half way before the panic. Still open:
   `cxk rig crashlog` reading the stick as a raw device, and the run on the
   rig itself. Before booting any machine but the rig, the install guard
   (2026-10-22): with `CXK_AUTO_INSTALL=0`, the default, the disk layer
   refuses every write to a disk but the one CXOS booted from, and
   `cxk verify-boot --foreign-disk` proves a drive ahead of it is untouched.
   That disk is named, not guessed: the image build stamps an id into its
   XBPT header, stage 2 hands the one it booted from over in CXBI
   (`boot_disk_id`, CXBI 2, XBPT 2), and the kernel writes only to the disk
   that carries it - to none if no disk does, or if two do. The foreign disk
   is the image itself under another id, so a machine whose own drive
   already holds CXOS is the case proven.
5. **The x86-64 kernel boots.** The BIOS loader enters long mode and starts a
   64-bit kernel in the top half (`kernel/CX_KERNEL_PLAN.md` §2.4), with NX, the 64-bit GDT, IDT and TSS
   (IST for the double fault) and `syscall`/`sysret`; the page database, the
   buddy allocator with a below-4 GB zone, and the direct map
   (`kernel/CX_KERNEL_PLAN.md` §9). The self-test suite
   passes, in QEMU and then on the rig.
6. **X for x86-64.** `usize` / `isize`, a target layer in both compilers, a
   64-bit code generator held to the C# oracle by the differential tests, and
   syscalls through the `syscall` instruction (`language/CX_X_PLAN.md`). The
   new ABI lands here, since every program is rebuilt anyway: one operation
   per syscall, fixed widths, the system page, interfaces generated from
   `.xfil` (`kernel/CX_SYSCALLS_IPC.md`). The
   executive starts and the full boot passes; the 32-bit kernel and code
   generator are then deleted.

   The ABI goes first (Aurora, 2026-10-26), while the 32-bit system still
   boots in full with the executive, so the new interface is proven end to end
   before any 64-bit code generator exists:

   - **6a1** The `.xfil` language and its generator in the DevKit, the
     `kernel` domain only. The first `.xfil` describes today's ABI unchanged;
     `cxk_abi.h` and the X prelude's constants, structures and syscall
     wrappers are generated from it, and `check-abi` requires the committed
     files to match. Every program compiles to the same bytes as before.
     **Done 2026-10-26** (`kernel/CX_SYSCALLS_IPC.md` §9.5): both kernels
     and every program byte-identical.
   - **6a2** The reshape: one operation per syscall, fixed widths, the new
     error list, a generated kernel dispatch table with argument decoding.
     The userland, the executive and `xc`'s host layer move to it, still on
     `int 0x80` on x86-32. Split in four (Aurora, 2026-10-26), each an ABI
     commit that boots in full; the calls keep today's meaning (paths
     relative to the cwd, `fb` and input until the display server), and
     directory handles, deadlines and memory objects come in their own steps:
     - **6a2a** The generated dispatch table and five-register gates; no ABI
       change, every program byte-identical. **Done 2026-10-26.**
     - **6a2b** The §6 error list. **Done 2026-10-26** (`kernel/CX_SYSCALLS_IPC.md` §6,
       `CXK_ABI_VERSION` 4).
     - **6a2c** One operation per syscall, a family at a time (memory, power
       and input, framebuffer, network, files). Memory done 2026-10-26
       (`CXK_ABI_VERSION` 5), power and input 2026-10-26 (6), framebuffer
       2026-10-26 (7), network 2026-10-26 (8), files 2026-10-26 (9, at
       0xA0-0xBF: eighteen calls did not fit 0x60-0x6F). **Done.**
     - **6a2d** Fixed widths: sizes and offsets `u64`. On x86-32 a `u64`
       argument takes a register pair (low, high), joined by the generated
       decoder; on x86-64 it is one register.
   - **6a3** The system page: the syscall stubs, the ABI hash recorded in the
     image and checked by the loader, and the kernel refusing a syscall from
     anywhere else.
   - **6b** `usize` and `isize` (a type of their own, Aurora 2026-10-26: mixing
     them with `u32` or `u64` needs `as`, even where the widths agree), the
     typed IR, and an x86-64 back end on it in the C# compiler, System V
     AMD64. The x86-32 emitter stays as it is until it is deleted.
   - **6c** `xc` gains the IR and the x86-64 back end; the differential tests
     and the self-hosting fixed point hold on x86-64.
   - **6d** The executive starts on x86-64 and the full boot passes; the
     32-bit kernel and code generator are deleted.

**Done when.** Each step closes only when these hold, each proven by a
sabotage that turns it red, on top of every existing self-test passing:

| Step | Done when |
|---|---|
| Before 1 | Kernel code with no hardware dependency builds and runs as host unit tests under AddressSanitizer and UBSan (`safety/CX_SAFETY.md` §3.7). **Done 2026-10-16.** |
| 1. Object model | A duplicated or transferred handle never has more rights than its source; a closed handle's number is refused, not reused silently; every object type's live count returns to its starting value after a stress run of create, duplicate and close; reference counts are atomic; every syscall passes the one recording point (a counter there equals the dispatch count); kernel objects come from per-type caches and the linked-list heap serves only what remains |
| 2. Processes | Two threads in one process share its handle table and address space; the thread and handle tables grow past today's 64 and 16; a process's exit releases every handle and frame (live counts return to their start) |
| 3. Arch layer | No file outside `kernel/arch/` contains CPU-specific code (an include check enforces it); the feature table is printed at boot; QEMU with a minimal and a full CPU model both pass. **Done 2026-10-18.** |
| 4. CXBI, crash log | Nothing reads a fixed boot address (the loader's old addresses are poisoned in a test build); a deliberate panic's log is recovered on the next boot, in QEMU and on the rig |
| 5. x86-64 kernel | The self-test suite passes in QEMU, minimal and full CPU models, then on the rig; NX, W^X and the guard pages are proven by tests that must fault |
| 6. X for x86-64 | The differential tests and the self-hosting fixed point hold on x86-64; the executive starts; the full boot passes |

Independent of the order above, and small enough to land whenever convenient:

- **Close the input hole:** `SYS_INPUT_READ`, `SYS_INPUT_POLL` and `SYS_MOUSE_READ`
  need no capability today, so any process can read keystrokes meant for another.
- A kernel random number generator: there is none today, and KASLR, the
  stack protector, TCP and keys all need one (`security/CX_SECURITY.md` §3.2).
- Stack protector on, seeded from it.
- Find the intermittent first-boot install failure (about 1 in 20 boots,
  `CXK_INSTALL_FORMAT_ERR`), using QEMU's whole-machine record and replay.

### Phase 1: processes, waiting and IPC

`kernel/CX_KERNEL_PLAN.md` §3–§7.

1. Process handles: wait, stop, kill, inspect; detailed exit reasons; jobs with
   memory and thread budgets.
2. The wait queue, and timers as handles (LAPIC timer and TSC).
3. IPC: queued calls with reply tokens, badges, handle transfer, sealable
   memory objects, notifications, priority passed through calls. Memory
   objects with pages on first touch, the region tree, and strict commit
   (`kernel/CX_KERNEL_PLAN.md` §9.4).
4. Pipes and explicit handle passing at spawn; the shell runs from disk.
5. User threads, futex (owner in the futex word), thread-local storage.
6. SMP readiness: spinlocks that save interrupt state, per-CPU data, a lock
   order checker in debug builds, the slab allocator. Then SMP on 8 cores.

### Phase 2: the GUI foundation

`system/CX_DISPLAY.md`, `kernel/CX_DRIVERS.md`.

1. Device tree and scored matching; today's drivers converted with no change
   in behaviour.
2. Driver-host primitives: register ranges as memory objects, interrupts as
   handles, DMA buffers.
3. The display server: back buffer, damage, virtual screen for CI.
4. Windows, transparency, server-drawn frames, focus and input routing.
5. Retained layers and spring animations in the compositor.
6. Trusted UI: the uninterceptible key combination, consent screens, signer
   in title bars, capabilities for capture and overlays.
7. Bochs display, then virtio-gpu 2D. The Process Manager.

### Phase 3: storage and the system tree

`system/CX_FILESYSTEM.md`.

1. VFS layer, RAM filesystem, directory handles.
2. CXFS v3: per-directory indexes, unlimited extents, copy-on-write,
   checksums, attributes, snapshots.
3. The new layout: read-only signed `/System` layers, `/Local`, per-app
   folders, the known-folders API, layered settings.
4. Packages as signed images; updates as layers; loader rollback.

### Phase B: the minimum viable OS

Carried from x86-32, now on 64-bit: X Data (done), FAT32 on the VFS (as a
user-space filesystem), X Shell and a terminal, the Process Manager, basic
apps. Networking as a user-space service: UDP and DHCP, then DNS, then TCP,
then IPv6, per-app permission, and connect by name with TLS
(`system/CX_NETWORK.md`). Audio (`system/CX_AUDIO.md`) joins here.

### Later

Vulkan through Venus on virtio-gpu; the native AMD driver; the IOMMU (QEMU
first; the rig's chipset has none); the ecosystem phase (package repository,
key distribution, in-place updates); swapping to disk, memory compression and
NUMA; the assembler and linker in X (stage 5),
multi-target from the start; then ARM.

---

## 4. Where CXOS aims to be better

Collected from the design documents, so the claims can be checked in one
place.

- **Input** reaches only the focused window; no app can log another's keys.
- **Trusted UI** that no app can draw over or fake, showing who signed each app.
- **Apps survive a compositor crash.**
- **Animation stays smooth when an app is busy**, because the compositor runs it.
- **Drivers in user space holding only their own device**, restartable.
- **Signed drivers limited to the devices they declare.**
- **Updates roll back by themselves** when a new system fails to boot.
- **Each app's files in its own folder**, with documents reached through a
  trusted picker.
- **The system tree cannot rot**, because `/System` is read-only signed layers.
- **No zombies, no signals, no implicit inheritance.**

---

## 5. Novel features, trimmed

Kept because each solves a real problem:

| Feature | Problem it solves | Where |
|---|---|---|
| Handle grant chain with revocation | Taking back authority that has been passed on and copied | Phase 0.1 |
| Snapshots and per-app undo | Recovering from an app or ransomware wrecking files | Phase 3.2 |
| Origin marker on downloaded files | Malware arriving by download | Phase 3.2 |
| Typed service interfaces | GUI first, never GUI only | Phase 2 onward |

Kept, smaller: process record and replay for developers (needs the recording
point from Phase 0.1; it does not help kernel bugs).

Deferred or dropped: compiler-derived permission lists (authority comes from
handles, so the list says little), on-device rebuild checks and a public
signing log (ecosystem phase), handles across machines (dropped).

---

## 6. Open questions

- What replaces each `docs/x86_32/` document as its area is reworked.
- ~~The ink polarity question in `devkit/CX_DEVKIT_DESIGN.md` §5.1.~~ Moot since 2026-10-16: fonts come from published BDF sources, which have no ink to interpret (`system/CX_FONTS.md`).
- Whether `setcheck.xuex` should write on every boot.
