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
| **x86-64 is the primary target, and comes first.** | The test rig is a 64-bit CPU with 4 GB (it had 8 earlier) (`hardware/CX_TARGET_HARDWARE.md`). Everything built for 32-bit before the move gets ported afterwards, so the move goes ahead of the IPC, display and driver work. |
| **PAE is not a separate step.** | Long mode includes NX and the four-level page tables that PAE was a stepping stone to. |
| **ARM is deferred until after the ecosystem phase.** | Nothing on the near path needs it, and the rig is x86. |
| **X and the ABI are designed for x86-64, AArch64 and RV64 now** (Aurora, 2026-10-30); a 32-bit port may follow, by choice. Each new X primitive is defined for every target, with what it lowers to on each (`language/CX_X_PLAN.md` §8); the ABI has no target-sized types. | The ports are later, but a primitive designed for x86 alone is found wrong only when a port starts, and x86's strong ordering hides missing barriers from every test run on it. |
| **x86-32 is retired.** The `x86_32_DEV` and `x86_32_RELEASE` branches stay as the reference. On this branch the 32-bit kernel and code generator are the working baseline only until their 64-bit replacements pass the full boot (Phase 0.6), then they are deleted. | The rig is 64-bit and ARM is far off; two x86 targets would double every kernel test for no user. A 32-bit port later starts from the reference branches. |
| **Built for any x86-64 CPU; features are detected at boot.** | The rig is the first test machine, not the target. Nothing is compiled for Piledriver: the kernel reads CPUID once at boot and every optional feature is used only where it exists (`kernel/CX_KERNEL_PLAN.md` §2.3). |
| **The kernel lives in the top half of the address space, with all physical memory mapped there.** | What every modern 64-bit kernel does; with 4 GB nothing needs mapping on demand (`kernel/CX_KERNEL_PLAN.md` §2.4). |
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
| **Memory: object caches by type, a page database with a buddy allocator, memory objects filled on first touch, strict commit.** | The heap, bitmap and eager mappings do not scale to gigabytes and 8 cores. Strict commit means running out is an error at the call that asked, never an OOM killer. Swapping, compression and NUMA are later (`kernel/CX_KERNEL_PLAN.md` §9). |
| **BIOS boot is the main path; UEFI is second.** | The rig has BIOS only. The BIOS loader must start a 64-bit kernel itself. |
| **GUI first, never GUI only.** | Every feature is a service with a typed IPC interface; the GUI and the CLI are both clients of it. The CLI remains the recovery and test path. |
| **Stop and kill are separate operations with separate rights.** | Stop asks politely and forces after a deadline; it is the everyday one. Kill is absolute, immediate and unrefusable, harder to obtain, and confirmed in the UI. |
| **The task manager is called the Process Manager.** | Clear, and it is what people search for. Revisit only if its scope outgrows processes. |
| **The display server draws window frames.** | Consistent frames, and a frozen app can still be moved and closed. |
| **The compositor runs retained layers and animations.** | Smooth animation even when an app is busy; the largest visible difference from the others. |
| **The first native GPU is AMD.** | The rig's GPU is a FirePro W5000. |
| **Novel features are kept only where they solve a real problem** (§5). | Several first ideas were novelty for its own sake; the trimmed list is what survives. |
| **The panic screen is friendly, and carries its essentials as a QR code** (Aurora, 2026-11-08; Phase 2 step 3a, before 2.4). | The rig may have no serial port, and x86-64's `kpanic` is still the minimal one (`TRANSITIONAL_CODE.md`), so a crash there leaves almost nothing. The screen says what happened in plain language, gives a stop code and crash number, and shows a short symbolised trace; the QR code lets anyone send the details without typing them. |
| **`setcheck.xuex` writes only when asked** (Aurora, 2026-11-08): behind a flag, off by default, on in the verification boots that test settings durability. | A boot that writes unasked changes a release image (which then fails its `SHA256SUMS`), wears the rig's disk, and hides any other write at boot. |
| **`x86_64-RELEASE` gets a ruleset** (Aurora, 2026-11-08): no deletion, no force-push, changes by PR only; no required checks (Actions is not used). | "PR only" is the rule already; nothing enforced it. |
| **X's safety model (`language/CX_X_PLAN.md` §5) is accepted, in stages** (Aurora, 2026-11-08): no undefined behaviour outside `unsafe`, `unsafe` blocks and checked conversions first; linear handles after 2.4; contracts at run time later, proofs much later. Each lands in both compilers with a Differential fact. | The first three are cheap and give the X tests a contract; linear handles are the largest compiler job and pay off once windows add handle types; leaked handles are this codebase's known failure. |
| **Kernel-stack slots stay 64 KB until stack depth is measured** (Aurora, 2026-11-08). A high-water-mark test (stacks filled with a pattern, the deepest use reported) comes first; 32 KB (512 threads) is the likely size after it. | 16 KB is what Linux uses, but nothing here has measured its deepest paths (USB, filesystem, IPC), an overflow is a double fault, and nothing needs 1024 threads yet. |

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
       decoder; on x86-64 it is one register. **Done 2026-10-26**
       (`CXK_ABI_VERSION` 10, `kernel/CX_SYSCALLS_IPC.md` §9.5): the 2 GB
       file cap is gone, seek and tell write the position through a `*u64`,
       and `SYS_FILE_LOCK` takes a `file_lock_args`.
   - **6a3** The system page: the syscall stubs, the ABI hash recorded in the
     image and checked by the loader, and the kernel refusing a syscall from
     anywhere else. Split in three (Aurora, 2026-10-26), each booting in full:
     - **6a3a** The ABI hash: SHA-256 of `abi/kernel.xfil` without its
       comments, all 32 bytes in the CXEX header (format 2, 88 bytes; the hash
       at offset 56). `abi_version` stays as the readable number. The loader
       refuses a different hash with `CXEX_LOAD_BAD_ABI_HASH`. **Done 2026-10-26.**
     - **6a3b** The system page and its stubs, one per syscall. The loader
       maps it at an address it picks and passes the base in a register at
       entry (Fuchsia's vDSO); `__syscall` becomes an indirect call through
       it, in both compilers and the C programs. **Done 2026-10-27**
       (`CXK_ABI_VERSION` 11, `kernel/CX_SYSCALLS_IPC.md` §9.5): the base in
       `esi`/`rsi`, a per-process copy under the stack, images kept below
       `USER_IMAGE_END`; `--no-prelude` keeps `int $0x80` for the Linux host.
     - **6a3c** The kernel refuses a syscall whose return address is not the
       stub for that number. **Done 2026-10-27** (no ABI change,
       `kernel/CX_SYSCALLS_IPC.md` §9.5): `syscall_check_origin` ahead of
       dispatch, a refusal ends the process as a fault does, and the kernel's
       ring-3 helpers share one counted system page in its space.
   - **6b** `usize` and `isize` (a type of their own, Aurora 2026-10-26: mixing
     them with `u32` or `u64` needs `as`, even where the widths agree), the
     typed IR, and an x86-64 back end on it in the C# compiler, System V
     AMD64. The x86-32 emitter stays as it is until it is deleted. Split in
     three (Aurora, 2026-10-28):
     - **6b1** `usize` and `isize` in both compilers' front ends (and as
       literal suffixes), as wide as a pointer, 4 bytes on x86-32. Any mix
       with another integer type needs `as` (initialising, assigning,
       passing, returning, arithmetic, comparing), except a constant that
       fits and a shift count. Type only: no program moves to it yet, so
       every program compiles to the same bytes. **Done 2026-10-28.**
     - **6b2** The typed IR, lowered from the checked tree, with a dump and
       tests; no back end on it yet. **Done 2026-10-28**
       (`devkit/CXEX.Lang/IR/IR.md`, `cxk ir`).
     - **6b3** The x86-64 System V back end on the IR, emitting ELF64, its
       programs run on Linux in the CI container. **Done 2026-10-28**
       (`devkit/CXEX.Lang/CodeGen/X64Emitter.cs`, `cxk compile --target
       x86_64`).
   - **6c** `xc` gains the IR and the x86-64 back end; the differential tests
     and the self-hosting fixed point hold on x86-64. In three steps, each a
     green commit:
     - **6c1** `xc` takes a target (`--target`; its pointer width and
       alignment replace `PTR_BYTES`), lowers to the IR and dumps it:
       `os/xc/ir.xfxn`, `irdump`, held to `cxk ir` on x86-32 and x86-64.
       **Done 2026-10-28.**
     - **6c2** `xc`'s x86-64 emitter on that IR, held to `cxk compile
       --target x86_64`'s assembly. **Done 2026-10-28** (`os/xc/x64.xfxn`,
       `asmdump --target x86_64` against `cxk asm --target x86_64`).
     - **6c3** `xc` runs as x86-64 (its host layer on `usize` and the
       `syscall` instruction); the fixed point and `tests/lang/run` hold there.
       **Done 2026-10-28** (`xc --target x86_64`; Linux's numbers per target
       in `os/xc/host/x86_32|x86_64/xc_linux.xfxn`; `SelfHostTests` on both
       targets). The heap still hands out `u32` addresses, so the host layer
       grants memory under 4 GB only; `std/heap` moves to `usize` with 6d.
   - **6d** The executive starts on x86-64 and the full boot passes; the
     32-bit kernel and the tree-walking x86-32 code generator are deleted.
     In steps, each a green commit:
     - **6d1a** The prelude per target: on x86-64 a syscall wrapper passes a
       pointer, a `u64` and an `i64` whole in one register (the decoder's
       `SYSCALL_PAIRS` 0), where the x86-32 wrappers cut them to a `u32` or a
       pair. `cxk prelude --target`; no ABI change, x86-32 byte-identical.
     - **6d1b** `std/heap`, `std/buf` and `list` on `usize`; the
       differential dump programs built for either target. **Done
       2026-10-28:** the heap hands out `usize` addresses (sizes stay `u32`;
       the arena header is 16 bytes on x86-64). `list` stays a list of `u32`
       values and a new `plist` holds addresses (Aurora, 2026-10-28: an index
       list stays 4 bytes an item). xc's host `vm_map` on x86-64 moves each
       grant above 4 GB with `mremap`, so a pointer cut to 32 bits faults. Every
       Differential comparison runs with the dump program built as x86-32 and
       as x86-64.
     - **6d2** The executive on x86-64, the full boot passing. **Done
       2026-10-28** in three commits: `cxk build` packages an ELF64 as arch 2
       (6d2a), the X Data reader linked into the x86-64 kernel (6d2b), and
       every program built for x86-64 and staged on its disk (6d2c). The
       executive, shell, supervisor and services run as on x86-32.
     - **6d3** The 32-bit kernel and the x86-32 emitter deleted, in three
       commits:
       - **6d3a** `verify-boot --full` requires the executive's banner and
         the shell's prompt, both written from ring 3, not only the kernel's
         "executive started" (logged once the image is loaded, so a broken
         x86-64 syscall wrapper stayed green). **Done 2026-10-28.**
       - **6d3b** The 32-bit kernel, its disk, its boot path and the x86-32
         program rules; x86-64 is the default architecture everywhere.
         **Done 2026-10-28:** `kernel/arch/x86_32/`, `pmm.c`, stage 2's CXEX
         path and both 32-bit images are gone; the build is one x86-64 build
         (`cx_x_program`); `verify-boot`, `cxk compile`, `cxk prelude` and
         `xc` default to x86-64. The dump verbs (`cxk asm`/`ir`/`sema` and
         xc's dump programs) keep x86-32 as their default until 6d3c, since
         the x86-32 emitter is their oracle pairing.
       - **6d3c** The tree-walking x86-32 code generators: `X86Emitter` and
         `xc`'s `emit.xfxn`, with xc's x86-32 host layer. **Done
         2026-10-28** (Aurora chose to delete `emit.xfxn` with its oracle):
         `cxk compile`/`asm`, `xc` and `asmdump` refuse `--target x86_32`;
         every verb and dump program defaults to x86-64, including
         `cxk sema`/`ir` and `semadump`/`irdump`; `ClangTool` targets
         `x86_64-elf` only. The Differential suites run each dump program
         as an x86-64 host only; the x86-32 asm comparison went with its
         subject, the x86-32 IR comparison stays. `LangRunTests` keeps the
         refusals (on x86-64); its run, interop and `std/buf` facts were
         already in `X64RunTests`.
     **Not in 6d:** the ABI keeps user addresses as `u32` (`vm_map_args.addr`,
     `spawn_args.image`, the IPC buffers, `vm_unmap`'s `addr`) and the kernel
     decodes a pointer argument as `uint32_t`. Nothing truncates while the
     x86-64 user half keeps x86-32's layout (images below `USER_IMAGE_END`,
     mappings in `VM_MMAP_BASE..VM_MMAP_TOP`); widening them is an ABI step
     of its own, before the user half grows past 4 GB. **Done in ABI 12
     (2026-10-28), as Phase 1's first step:** every user address in a
     structure or argument is a `u64`, the kernel checks it whole against
     `USER_VLIMIT`, and a hint or buffer 4 GB above a valid one is refused
     rather than cut down to it. `ipc_recv_args.sender` turned out to be an
     address too. Still 32-bit: a CXEX section's virtual address (the image
     format) and the user layout itself, which stays below 4 GB.
     **X stays cross-architecture:** the `x86_32` target stays in both front
     ends and the IR, and an x86-32 back end on the IR (like `X64Emitter`)
     can come back later without touching the language.

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
  needed no capability, so any process could read keystrokes meant for another.
  Done in Phase 1 step 7 (1.7a, `GRANT_INPUT`).
- A kernel random number generator: there is none today, and KASLR, the
  stack protector, TCP and keys all need one (`security/CX_SECURITY.md` §3.2).
- Stack protector on, seeded from it.
- Find the intermittent first-boot install failure (about 1 in 20 boots,
  `CXK_INSTALL_FORMAT_ERR`), using QEMU's whole-machine record and replay.

### Phase 1: processes, waiting and IPC

`kernel/CX_KERNEL_PLAN.md` §3–§7.

1. Process handles: wait, stop, kill, inspect; detailed exit reasons; jobs with
   memory and thread budgets. In four steps:
   - **1.1a (ABI 13, 2026-10-29), done:** `spawn` and `exec_path` return a
     process handle (`HRIGHT_WAIT`, `HRIGHT_INSPECT`, `HRIGHT_DUPLICATE`), not
     a pid. `SYS_OBJECT_WAIT` waits on one handle's signals with a timeout
     (`HRIGHT_WAIT` is generic; the deadline form and the queue come with
     item 2). A process's last thread sets `SIGNAL_PROCESS_ENDED` on its
     way out, and `OBJINFO_PROCESS` says how it ended: exited with a code,
     crashed (vector, where, the page-fault address; a refused call is
     `PROC_VECTOR_REFUSED`), or never started. No zombies: the record and
     its pid go with the last handle. The supervisor uses it to skip a
     scheduled task whose last run is still going.
   - **1.1b (ABI 14, 2026-10-29), done:** `SYS_PROCESS_KILL` on a handle with
     `HRIGHT_KILL`. A blocked thread (IPC call and receive, file lock,
     keyboard, sleep, object wait) is woken, and every thread ends at its
     next safe point: the syscall gate's way out, or an interrupt taken in
     ring 3. It ends `PROC_KILLED`, `killer` the pid that asked.
   - **1.1c (ABI 15, 2026-10-29), done:** `SYS_PROCESS_STOP` on a handle
     with `HRIGHT_STOP` (handed out freely) leaves a `stop_notice` on the
     endpoint the process registered (`SYS_PROCESS_STOP_ENDPOINT`), which
     `ipc_recv` returns ahead of any call; still running at the deadline, it
     is ended as a kill ends it, `PROC_STOPPED`. Nothing registered, or
     nobody left to receive: ended at once.
   - **1.1d (ABI 16, 2026-10-29), done:** jobs. `SYS_JOB_CREATE` nests one
     below another (at most 8 deep); `spawn_args.job` puts a process in one
     (`HRIGHT_ASSIGN`). Committed memory and threads are charged to the job
     and every job above it, all or none, so a subtree of processes is held
     to one budget; `SYS_JOB_SET_LIMITS` changes it, `OBJINFO_JOB` reads it,
     and `SYS_JOB_KILL` ends every process below and closes the subtree
     (`SIGNAL_JOB_ENDED` when the last is gone).
2. The wait queue, and timers as handles (LAPIC timer and TSC). In three steps:
   - **1.2a (ABI 17, 2026-10-29), done:** the monotonic clock in nanoseconds
     (`SYS_CLOCK_MONOTONIC`); `SYS_OBJECT_WAIT` takes an absolute deadline on
     it (`DEADLINE_NEVER` for none); timer objects (`SYS_TIMER_CREATE`,
     `SYS_TIMER_SET`, `SYS_TIMER_CANCEL`) fire once at a deadline,
     `SIGNAL_TIMER_FIRED`, from one deadline-ordered list the tick looks at
     the head of. Still on the 1 ms PIT tick.
   - **1.2b (ABI 18, 2026-10-29), done:** the queue object
     (`docs/kernel/CX_KERNEL_PLAN.md` §5): `SYS_QUEUE_CREATE`,
     `SYS_QUEUE_REGISTER` a handle with a key once, one-shot and re-armed by
     `SYS_QUEUE_ARM`; `SYS_QUEUE_WAIT` returns only the keys that became ready,
     until a deadline; `SYS_QUEUE_UNREGISTER`. Calls 0x60-0x6F.
   - **1.2c (ABI 19, 2026-10-29), done:** the LAPIC timer programmed for the
     next deadline instead of a fixed tick, and the clock from the TSC; the
     PIT stays the fallback. Timer slack comes with it:
     `SYS_TIMER_CREATE(slack)` (`docs/kernel/CX_SYSCALLS_IPC.md`). Idle sleeps
     until something is due. Not yet: the clock on the system page (`CX_SYSCALLS_IPC.md` §8).
3. IPC: queued calls with reply tokens, badges, handle transfer, sealable
   memory objects, notifications, priority passed through calls. Memory
   objects with pages on first touch, the region tree, and strict commit
   (`kernel/CX_KERNEL_PLAN.md` §9.4). In six steps:
   - **1.3a (ABI 20, 2026-10-29), done:** calls queue on the endpoint in
     order (`SIGNAL_EP_READABLE` while one waits). `SYS_IPC_RECV` hands back
     an `ipc_msg_info` (badge, length, reply capacity, sender) and a reply
     token, a handle with `HRIGHT_REPLY` that `SYS_IPC_REPLY` answers, in any
     order. Callers hold send handles (`OBJ_TYPE_SEND`), which
     `SYS_EP_MINT` makes with a badge and rights that only shrink. A call's
     buffer is charged to the caller's job. A closed endpoint fails every
     queued call, a closed token fails its caller, and a killed caller's
     queued call leaves the queue (`docs/kernel/CX_SYSCALLS_IPC.md`).
   - **1.3a′ (2026-10-29), done:** stage 2 gathers the kernel at 16 MB, not
     below 1 MB: 32 KB `int 13h` chunks land at 0x10000 and are copied up in
     unreal mode, re-entered after every `int 13h` (a BIOS may reset FS's
     limit). The 16 MB buffer is checked against E820 and the kernel's
     segments must end below it. The budget is 4 MB (`KERNEL_SECTORS`, held
     equal to the image writer's by a DevKit test); the writer patches the
     kernel's real size into stage 2, so it reads only that. The
     `CXK_TEST_HOSTILE_BIOS` build plays a BIOS that leaves 64 KB limits.
     `cxk verify-hyperv` boots the image in a Hyper-V Generation 1 VM (AMI
     BIOS, IDE, COM1 to a pipe) and judges it as `verify-boot` does. The rig
     stays the final word.
   - **1.3b (ABI 21, 2026-10-29), done:** handles in messages: a request
     and a reply each carry up to 16, moved (keeping their grant) or copied
     (`HRIGHT_DUPLICATE`, a new grant derived from the source's) with rights
     that only shrink. Every entry is checked before any is taken, so a
     refusal moves nothing; the handles received and the reply token land
     together or the call is requeued. A handle in flight keeps its
     object's handle count. The endpoint or token a message goes through
     cannot travel (`E_NOT_SUPPORTED`). Queued calls that fail close what
     they carried (`docs/kernel/CX_SYSCALLS_IPC.md` §7).
   - **1.3b′ (2026-10-29), done:** open files travel. Their locks stay
     owned by the open file (XNU OFD locks, Serenity, Haiku flock; not NT's
     file and process), so the receiver can unlock them. Each lock keeps
     the thread that took it for the deadlock check, made unknown when the
     handle travels (`kobj_ops.travel`); the check passes over an unknown
     taker, so no false `E_DEADLOCK`, and a real cycle through it hangs.
   - **1.3c (ABI 22, 2026-10-29), done:** notifications: a 64-bit word that
     `SYS_NOTIFY_SIGNAL` ORs bits into and `SYS_NOTIFY_TAKE` reads and
     clears, `SIGNAL_NOTIFY_PENDING` while any bit is set, so it is waited
     for through `object_wait` or a queue (seL4). Signallers, minted with
     `SYS_NOTIFY_MINT`, set only their own bits; once the notification's
     last handle closes they get `E_PEER_CLOSED`.
   - **1.3d (ABI 23, 2026-10-29), done:** memory objects: `SYS_MEM_CREATE`
     makes one, `SYS_VM_MAP` maps a view of it (or `MOBJ_ANON`), and pages
     arrive on the first touch, through the page fault or a kernel copy's
     pointer check (Serenity `AnonymousVMObject`, Haiku `VMCache`). Strict
     commit: the whole size is charged to the job and reserved in the page
     allocator, so running out is `E_NO_MEMORY` from the call. Regions are
     in an AVL tree (256 per process); mappings borrow the object's pages.
   - **1.3e (ABI 24, 2026-10-29), done:** sealing, and memory objects in
     IPC. A memory object's handle travels like any other, its commit still
     charged to the job that made it. `SYS_MEM_SEAL` (`HRIGHT_SEAL`) adds
     seals that bind every holder (Linux memfd): `MEM_SEAL_WRITE` means no
     writable mapping, refused with `E_BUSY` while one stands (the object
     counts them), and `MEM_SEAL_SEAL` ends the adding. A receiver reads
     them in `mem_info.seals`, so it can trust bytes it has checked.
   - **1.3f (ABI 25, 2026-10-29), done:** priority passed through calls.
     Threads have levels 1-15 in three classes (background, normal,
     real-time behind `GRANT_REALTIME`); the highest ready level runs,
     round-robin within it. `SYS_THREAD_SET_PRIORITY` sets a thread's own,
     capped by `job_limits.priority`. A server runs at least at the level
     of the calls it holds and of those queued on its endpoint, passed down
     chains of calls (XNU turnstiles), and the lift goes with the reply,
     the token, the caller or the endpoint.
4. Pipes and explicit handle passing at spawn; the shell runs from disk. In
   three steps:
   - **1.4a (ABI 26, 2026-10-29), done:** pipes. `SYS_PIPE_CREATE` makes a
     read end and a write end over one ring charged to the caller's job (XNU's
     pipe pair); `SYS_FILE_READ` and `SYS_FILE_WRITE` take either, with the
     handle's `HRIGHT_READ` or `HRIGHT_WRITE` as the authority, no
     `GRANT_DISK`. A read returns what is there, 0 once the writer has gone;
     a write of up to `PIPE_ATOMIC` bytes goes in whole, and with no reader
     is `E_PEER_CLOSED`, not a signal. Readable, writable and peer-closed
     signals for the queue (`docs/kernel/CX_SYSCALLS_IPC.md` §7).
   - **1.4b (ABI 27, 2026-10-29), done:** handles named at spawn.
     `spawn_args` lists up to 16 `spawn_handle` entries, each moved or
     copied as in a message into a numbered slot (XNU `posix_spawn` file
     actions with `POSIX_SPAWN_CLOEXEC_DEFAULT`, `PSFA_DUP2` naming the
     descriptor; not NT's or Haiku's inherit-by-flag), and the child starts
     with those and no others. It asks for a slot with `SYS_ARGS_HANDLE`;
     0-2 are standard input, output and error. The list is checked before
     the image and taken after it, so a refused spawn takes nothing. The X
     library's `puts`, `write` and `putc` write to the output handle when
     there is one (`docs/kernel/CX_SYSCALLS_IPC.md` §7).
   - **1.4c (2026-10-30), done:** the shell from disk. The executive starts
     `/System/Programs/shell.xsex` with `SYS_EXEC_PATH`, so the kernel
     verifies its signature like the supervisor's, and its embedded copy
     (`app_image.h`) is gone. The X library reads standard input from the
     `ARGS_STDIN` handle when there is one (no echo; its end ends
     `read_line`), so a shell given a pipe runs it as a script and exits at
     its end. `run` copies the shell's slots 0-2 into each program, and
     `run a | b` (up to four) moves a pipe's ends between neighbours, so the
     shell holds neither. `upper` is the first filter. No ABI change
     (`docs/kernel/CX_SYSCALLS_IPC.md` §7).
5. User threads, futex (owner in the futex word), thread-local storage.
   Split, in order:
   - **1.5a (2026-10-30), done:** user threads. `SYS_THREAD_CREATE` starts
     a thread with a kernel-made stack (a guard page below it, charged to
     the job, given back before its handle signals) and returns a thread
     handle; join is a wait on it. `SYS_THREAD_EXIT` ends one thread, the
     last one's code ending the process; `SYS_EXIT` and a fault end every
     thread. X gains `std/thread.xfxn` (spawn, join, exit) and
     `std/time.xfxn`. ABI 28 (`docs/kernel/CX_SYSCALLS_IPC.md` §7).
   - **1.5b**, in two (Aurora, 2026-10-30: X had no atomics, and a ring-3
     mutex needs compare-and-swap):
     - **1.5b1 (2026-10-30), done:** atomic intrinsics in X
       (`__atomic_load`/`_store`/`_xchg`/`_add`/`_cas` on a 4- or 8-byte
       integer through a pointer, sequentially consistent), one IR
       instruction (`atomic`), in the C# compiler and `os/xc`; the corpus
       programs `tests/lang/run/atomics.xfxn` and
       `tests/lang/refuse/atomics.xfxn` hold the Differential facts to them
       (`docs/language/CX_X_PLAN.md` §4). No ABI change.
     - **1.5b2 (2026-10-30), done:** futex keyed by memory object and
       offset (by process and address for a word outside any mapping:
       Serenity `get_futex_key`, XNU `ULK_UADDR`/`ULK_XPROC`), the owner's
       thread id in the word; `SYS_THREAD_ID`, `SYS_FUTEX_WAIT`,
       `SYS_FUTEX_WAKE` (0x0D-0x0F); X mutex, once and condition variable
       (`std/sync.xfxn`), and the heap and the output locked once a second
       thread starts. Input and the rest of the library stay one thread's.
       ABI 29 (`docs/kernel/CX_SYSCALLS_IPC.md` §7).
   - **1.5c**, in two (Aurora, 2026-10-30: X could not read through FS;
     userland makes each thread's block, the kernel only holds and switches
     FS_BASE, as Serenity's `archctl` and XNU's `thread_set_tsd_base` do):
     - **1.5c1 (2026-10-30), done:** `__tls_base()` in X, a `usize` read
       from `%fs:0`, the block's word naming itself (no FSGSBASE on the
       FX-8350); one IR instruction (`tlsbase`), in the C# compiler and
       `os/xc`. `tests/x64/run/tls.xfxn` sets FS with `arch_prctl` on the
       Linux host; `tests/x64/run` joins the Differential and self-host
       corpora (`docs/language/CX_X_PLAN.md` §4). No ABI change.
     - **1.5c2 (2026-10-30), done:** FS_BASE per thread, written on every
       switch; set by `SYS_THREAD_CREATE`'s `tls` and `SYS_THREAD_SET_TLS`
       (0xD0), a base outside the user half refused; `SYS_THREAD_SET_NAME`
       (0xD1) for the calling thread; CPU time charged at each switch
       (Serenity's way, not WRK's tick sampling); `OBJINFO_THREAD` gains
       `cpu_ns` and `name`. X: `std/tls.xfxn` (a block per thread, the
       first thread's a global, a spawned thread's a page it gives back), and
       `std/sync.xfxn` reads the thread id from it. ABI 30
       (`docs/kernel/CX_SYSCALLS_IPC.md` §7).
   - **1.5d**, in three (the results a machine reads come first, so the
     runner's tests report through them):
     - **1.5d1 (2026-10-30), done:** one structured line per self-test,
       on serial only and never wrapped: `@ktest begin`, then
       `@ktest result <index> pass|fail <ms> <name>` for each, then
       `@ktest end <passed> <total>`, tab-separated (XNU's tagged lines,
       `osfmk/tests/ktest_emit.c`; Serenity's LibTest prints a line per
       case too, `TestSuite::run`). `cxk verify-boot` fails a summary the
       lines don't agree with (a result missing, misnumbered or failed,
       no end line), names failed tests in its reason, and `--json` prints
       the verdict and every result, a failed one with the lines it logged.
       No ABI change.
     - **1.5d2 (2026-10-30), done:** a ring-3 X test runner,
       `os/std/test.xfxn`, and the X tests, `os/apps/xtest.xfxn`. Each test
       is a thread function (0 a pass, else the step it failed at) run on a
       thread of its own and joined with a deadline: a hang fails at its
       deadline and stops the run (the hung thread may hold the heap's or
       the output's lock, and no thread can be ended from outside); a fault
       ends the process, failing the test it was in. The runner first names
       every test, then runs them, writing `@xtest case|start|result|end`
       lines to its output pipe with SYS_FILE_WRITE directly (no lock, no
       heap). The kernel's `ktest_xtests` reports each result as a self-test
       of its own (`x: <name>`), each test left without one as failed in its
       place, so a red boot has the same tests as a green one, and then
       "the runner ran cleanly" (exit 0, every line in order, nothing left
       behind); its 600 s deadline backs up the runner's. Serenity prints a
       START line per test for the same reason (`run-tests.cpp`,
       `do_run_single_test`); XNU forks a child per test
       (`tests/vm/vm_allocation.c`, `child_test_passed`), which keeps a crash
       to its test: CXK has no fork. threads.xfxn's and futex.xfxn's plain
       checks moved in as 17 tests; what only a process's end shows (a
       worker's SYS_EXIT, a crash, the first thread ending first, a futex
       waiter's process ending) stays in those programs. Self-tests 168.
       No ABI change.
     - **1.5d3 (2026-10-30), done:** `cxk test all` (the OS built and
       booted plain and `--cpu max`, `dotnet test` with the kernel host
       tests, the container's Differential run, each read from JSON or TRX,
       one summary; a skipped step is never a pass) and `cxk test sabotage
       <file>|--all`, which owns the backup, restore, pattern counts and
       tree checks every step's hand-written matrix script got wrong at
       least once, refuses to judge against a red baseline or a failed or
       stale build, and judges a sabotage by which `@ktest` results went
       red, and at which step (`devkit/CX_CXK_PLAN.md` §3.4). Sabotages are
       recorded in `tests/sabotage/<step>.sab`; 1.5d2's three are the
       first, re-run by the command with the hand-run verdicts. In the
       working tree with crash-safe backups, not a scratch copy as first
       planned (the build reads gitignored keys and caches). From 1.6 on,
       each step's sabotages go in a `.sab` file instead of a script.

   Order from here (Aurora, 2026-10-30, after an outside review: kernel
   correctness work stays ahead of the display server and is not traded
   for it):
6. Concurrency and failure testing. Test hooks in test builds: race
   points (a thread parked, or a kill or close delivered, at a named
   point), failing the Nth allocation (`kmalloc`, `ocache_alloc`,
   `pmm_alloc`, `kstack_alloc`, `vm_map`), and live counts (each object
   cache, free pages, handles, mapped bytes, the job's charge) compared
   before and after. Then sweeps of every syscall's failure paths, resource
   limits driven to both sides of the bound, and the futex, process-ending,
   IPC and object-lifetime races that HANDOFF lists as untested. In three,
   as 1.5d was (each later part is built with the tools of the one before):
   - **1.6a (2026-10-30), done:** the hooks, `kernel/diag/testhook.h`,
     behind `CXK_TEST_HOOKS` (on; 0 compiles them out and their self-tests
     fail, never skip). Failing the Nth allocation at a set of the five
     sites, counted for the arming thread only, so a timer or helper
     allocating meanwhile neither takes the failure nor moves N. Live
     counts: one snapshot of free and reserved pages, heap bytes and
     blocks, kernel stacks, threads, processes, jobs, every object type,
     every object cache (now on one list, `ocache_each`), and for a process
     and a job their handles, mapped bytes and charges; `live_diff` names
     each count that moved. Race points: a named place where the Nth
     arrival of one thread (or any) is parked until the test releases it,
     or calls the test's function there, once per arming; one reached
     holding a spinlock is a panic (1.6c1; at first, one reached with
     interrupts off). XNU names its injection points as stages
     with one armed at a time (`osfmk/kern/upsi.h`,
     `check_for_failure_injection` in `osfmk/kern/debug.c`); Serenity reads
     its heap's numbers in one call (`Kernel/Heap/kmalloc.cpp`,
     `get_kmalloc_stats`); a search of WRK-v1.2 and Haiku's kernel found no
     fault injection; fail-the-Nth is Linux's `fail_nth`, from memory. Four
     self-tests (172): each hook against its own rules, and the first
     sweeps, failing in turn every allocation of a pipe, a memory object, a
     job, a process, a held thread and a mapping (4, 5, 1, 1, 8 and 8
     allocations): each run refuses and leaves every live count as it was,
     and a sweep that reaches no allocation fails. Real race points arrive
     with the tests that use them (1.6c). Sabotages: `tests/sabotage/1.6a.sab`,
     15 of 15 caught, three of them leaks planted in unwind paths (a pipe's
     job charge, a mapping's memory object, a kernel stack's pages). No ABI
     change.
   - **1.6b (2026-10-30), done:** the syscall failure sweeps from ring 3,
     and limits on both sides. `usermode_run_calls` makes three calls from
     ring 3 on the test thread, through the system page's stubs and the
     gate as a process does, with a data page the calls point into and
     `fault_arm` covering ring 3 only (the arming is per thread, so it
     crosses the gate with the thread). Two self-tests in
     `kernel/ktest_sweeps.c`: a census of every syscall in the table but
     six (EXIT, THREAD_EXIT, INPUT_READ, the power calls), each with every
     argument 0 and every argument hostile (a kernel address, a huge u32, a
     negative handle), holding no grant, unarmed and then with its first
     allocation failed: none answers E_INTERNAL or faults, a refusal
     reaches no allocation and refuses the same armed, a failed allocation
     answers E_NO_MEMORY, nothing is left behind, and the count called is
     the table's; and sweeps of 13 syscalls that make a handle, a
     registration or a mapping (pipe, endpoint and its mint, notification
     and its mint, timer, queue and a registration, job, memory object,
     anonymous and object mappings, file open), every allocation failed in
     turn as 1.6a's sweeps do. HANDLE_DUPLICATE allocates only when the
     table grows, and THREAD_CREATE is refused to a kernel process, so
     neither is swept here. Limits: four X tests (a pipe of PIPE_SIZE_MAX
     takes that many bytes; a memory object of MMAP_MAX_BYTES; thread
     stacks MIN - 1, MAX and MAX + 1; a process's handles to the refusal,
     one more after a close), and in the file test the argument page
     (USER_ARGS_MAXC and one more, a blob filling the page with its header
     and one byte more), FILE_PATH_MAX and FILE_NAME_MAX each side. **Bug
     fixed:** spawn checked only the blob's length, so 65 arguments, or a
     blob that fit the page without its header, were spawned with no
     arguments at all (`build_args_page` drops what does not fit, and its
     comment said the callers made that unreachable); `args_fit` now
     refuses them in `check_args` and `proc_start` (E_OUT_OF_RANGE).
     **Changed, asked:** a handle table whose growth cannot get memory now
     answers E_NO_MEMORY (`HTAB_E_NO_MEMORY`, from `grow` through install,
     dup and land); a full table still answers E_NO_RESOURCES. Before,
     `htab_install` answered NO_RESOURCES whatever `grow` said, and IPC and
     file open remapped any failure to it. Left for 1.6c: IPC_MSG_MAX accepted (it needs a caller
     thread and 64 KB buffers; the X runner holds no endpoint grant).
     Self-tests 178. Sabotages: `tests/sabotage/1.6b.sab`. No ABI change.
   - **1.6c:** race points in the kernel and the races HANDOFF lists as
     untested, in three, one area each: 1.6c1 futex, 1.6c2 IPC, 1.6c3
     process ending and object lifetime.
   - **1.6c1 (2026-10-30), done:** the futex races. Two race points in
     `futex_wait`: `RP_FUTEX_READ` (the word faulted in and keyed, not yet
     read) and `RP_FUTEX_WOKEN` (the block over, the lock not yet
     retaken). Three self-tests in `kernel/ktest_races.c`, the wait made
     from ring 3: a word unmapped at the first point answers E_BAD_ADDRESS,
     leaves no waiter and drops the key's reference on the memory object
     (live counts); a waiter whose deadline has come, woken at the second
     point, answers E_OK and the wake counts it; one memory object mapped
     in two processes at different addresses is one word, and another
     object at the same offset is not. XNU's ulock settles a wake against
     a timeout with one wait result under the waitq lock
     (`bsd/kern/sys_ulock.c`, `wait_result_to_return_code`); its
     `tests/ulock.c` (`test_waiter_with_timeout`) tests a plain timeout
     only. Race points no longer need interrupts on: a syscall's whole
     body runs with them off (FMASK clears IF), so 1.6a's check panicked
     at the first point inside a syscall. It now panics only when a
     spinlock is held (`spin_held`, counted in `sync/spinlock.h` when the
     hooks are built in). The ring-3 test helpers share the kernel's
     address space (only spawn gives a process its own), so the
     cross-process test keeps the two mappings apart by address
     (MMAP_HINT); the key is the object, never the address. Self-tests
     181. Sabotages: `tests/sabotage/1.6c1.sab`, 5 of 5. No ABI change.
   - **1.6c2 (2026-10-31), done:** IPC and queues. Two race points:
     `RP_IPC_REPLY_COPY` (a reply REPLYING, its bytes not yet read) and
     `RP_QUEUE_WOKEN` (a queue waiter woken, the wait lock not yet taken).
     Six self-tests in `kernel/ktest_races.c`, callers from ring 3 and the
     test thread serving: a token closed by another thread as the reply
     reads its bytes (a readable reply still reaches the caller; an
     unreadable one fails it with E_PEER_CLOSED, not a wait for good);
     IPC_MSG_MAX each way, a byte short and a byte long refused on each
     side; an endpoint closed and freed under a call it gave out (the call
     stops naming it, and a new endpoint, maybe in its slot, lifts nobody
     through it); a server thread that passed its token on ends (the call
     no longer names it as server, the token still answers); a queue's only
     event taken by another waiter between a waiter's wake and its look (it
     waits again and times out, never returns 0 events: "two threads racing
     one key" read as two waiters for one ready key); and a queue waiter
     killed while blocked, and killed while parked at the point, each
     leaving at once rather than at its deadline. XNU resumes a kevent wait
     with its wait result, THREAD_INTERRUPTED becoming EINTR
     (`bsd/kern/kern_event.c`, `kqueue_scan_continue`); Serenity marks a
     blocker interrupted by death (`Kernel/Tasks/Thread.h`,
     `set_interrupted_by_death`); Haiku's ports allow 256 KB
     (`PORT_MAX_MESSAGE_SIZE`, `src/system/kernel/port.cpp`).
     `usermode_calls_set` fills in a handle installed in a held helper
     process's table. Self-tests 187. Sabotages: `tests/sabotage/1.6c2.sab`,
     13 of 13 (four of them also red in the older IPC, priority and kill
     tests, which cover the same bounds). No ABI change.
   - **1.6c3 (2026-11-04), done:** process ending and object lifetime.
     One race point, `RP_SPAWN_TAKE` in `proc_start` (the named handles
     checked by the syscall, not yet taken). Two steps of the spawn test
     (`spv_handles` in `kernel/ktest_loader.c`): a named handle closed at
     the point, as another thread of the caller would, refuses the spawn
     with E_BAD_HANDLE, takes nothing (the moved handle stays), and leaves
     every live count as before the handle was made (no child, no process
     handle). Two self-tests in `kernel/ktest_races.c`: a refused vector
     in one thread of a process while another is blocked receiving (the
     refusing thread is made by SYS_THREAD_CREATE at the syscall
     instruction inside the SYS_GETPID stub, eax 0; its creator, raised
     above it, blocks first): the process ends PROC_CRASHED by
     PROC_VECTOR_REFUSED and the blocked thread with it; and a process
     holding the only handle on one end of a pipe, blocked reading an empty
     pipe and then writing a full one, killed: each leaves at once, the
     other end is told PEER_CLOSED, a write there is refused, and a read
     gets the bytes there were, none of the killed write's, then the end.
     Serenity notes the crash before ending the threads, the order the
     refusal keeps (`Kernel/Tasks/Process.cpp`, `Process::crash`, then
     `die()`). Self-tests 189. Sabotages: `tests/sabotage/1.6c3.sab`, 7 of
     7; two that leak (a child, a spinning pipe waiter) also redden the
     next test that counts globally, named as such. Not covered: SYS_PIPE_CREATE's
     cleanup when the ends cannot be written after the pointer check (left
     by 1.4a; now reachable with a race point). No ABI change.
7. Input authority and kernel randomness. A grant on `SYS_INPUT_READ`,
   `SYS_INPUT_POLL` and `SYS_MOUSE_READ` until the display server routes
   input. A ChaCha20 generator with Fortuna-style pools, fed by RDSEED or
   RDRAND where present, the TSC at every interrupt, timing jitter at boot,
   and a seed file on the boot disk rewritten at each boot; never blocking
   after boot. It also seeds the chaos mode of 6 (random yields at race
   points, reproducible from a printed seed) and the syscall fuzzer.
   In three: 1.7a the input grant (ABI 31); 1.7b the generator, its
   sources and `SYS_RANDOM_READ` (the rig's FX-8350 has neither RDRAND nor
   RDSEED, so the TSC samples and the seed file carry it there; no
   deterministic fallback, unlike Serenity's RTC one in
   `Kernel/Security/Random.cpp`); 1.7c the seed file and seeded chaos.
   - **1.7a (2026-11-04), done:** `GRANT_INPUT` (0x0400, in
     GRANT_OS_BASELINE) gates the three input calls, checked before
     anything is read, so a refused poll consumes no key and a refused
     read does not block; the pointer is refused before its address is
     looked at. `proc_start` drops it from every background process,
     SPAWN_BACKGROUND asked for or inherited, so the supervisor and its
     services cannot read the keyboard. The shell gives it only to the
     first program of a foreground `run` line, and only when the shell
     has no input handle of its own to pass (the child would read keys);
     pipe-fed programs and `&` lines get none. Tests: the input test now
     runs refused with every other grant held, then granted; the spawn
     suite spawns hi.xuex asking for it in the foreground, with
     SPAWN_BACKGROUND, and from a background parent, and reads the grants
     each process held; the two kill-in-keyboard-wait tests give their
     victim the grant (refused, it would not block). The census still
     skips INPUT_READ: with its gate broken it would wait for good, not
     go red. Until the display server routes input, whoever holds the
     grant reads every key. **Found on the way:** the executive's CMake
     rule did not depend on `cxk_abi.h`, so it kept the old baseline while
     `cxk build` stamped it ABI 31; the shell it started was refused the
     keyboard and re-prompted for good, and `verify-boot --full` passed on
     the prompt. Fixed (the dependency), and a full boot now fails on any
     grant refused after the executive starts, read again a second after
     a pass. The input test also drains the keyboard on every exit: a key
     left by a failed step turned the kill test's keyboard wait red.
     Untested: the shell's policy (which `run` program gets the grant) has
     no boot test; the shell-streams test runs it with GRANT_SPAWN only.
     Self-tests 189. Sabotages: `tests/sabotage/1.7a.sab`, 5 of 5.
   - **1.7b (2026-11-04), done:** `SYS_RANDOM_READ` (0xE0, ABI 32,
     unprivileged, at most RANDOM_READ_MAX = 256 bytes a call). Fortuna
     with 32 SHA-256 pools over a ChaCha20 generator (RFC 8439) with fast
     key erasure (`kernel/lib/crypto/`); one boot hash seeds it at 256
     credited bits, and until then the call answers E_SHOULD_WAIT and
     writes nothing: no fallback seed. Sources: RDSEED/RDRAND (64 bits a
     plausible word), boot jitter (1 bit a sample whose first three
     differences are all at least 16 cycles), and every interrupt, which
     stores {TSC, vector} in a 128-entry ring hashed in on the next
     request (XNU's split: nothing is hashed in the IRQ path). The loader's
     `rng_seed` is mixed in uncredited. Design: `docs/security/
     CX_RANDOM.md`. **Found on the way:** the timer is deadline-based, so
     an idle 20 ms gives one interrupt, not twenty; and QEMU's `-icount`
     still credits the jitter loop 256 bits, so default verification boots
     are seeded but replayable (the doc says so). Tests: 7 host tests (RFC
     vectors, the reseed schedule, the 256-bit rule, key erasure, the
     pool-0 and interval rules, the estimator, the CPU-word check), each
     seen red under a hand sabotage (14, listed in the commit); 4 boot
     tests (the cipher as built, interrupt samples reaching the pools, the
     call from ring 3 against a held generator, the boot seed's CPU
     credit under `--cpu max`); an X test (steps 47-51). Untested: what
     the rig's jitter earns (no RDRAND there) until it boots on the rig.
     Self-tests 194. Sabotages: `tests/sabotage/1.7b.sab`, 9 of 9.
   - **1.7c (2026-11-04), done:** the seed file, 64 bytes in the crash
     partition (an XCRL record of kind 5 at sector 48, CRC-sealed).
     Read once a boot after the disks; rewritten before it counts (from
     the generator if seeded, else SHA-256 of a label, a counter and the
     old seed), and credited 256 bits only once the rewrite reads back
     byte for byte, so a disk that drops writes never replays a credited
     seed. Rewritten from the generator once seeded (the idle loop's
     refresh). The loader's `rng_seed` stays uncredited. Chaos at race
     points: `chaos_permille`/`chaos_seed` in `kernel.xkco` make every
     race point yield 1-3 times at that rate, from a splitmix64 sequence
     whose 32-bit seed the boot prints for replay. Design: `docs/
     security/CX_RANDOM.md` §5, §8 (XNU `PE_get_random_seed`, Serenity
     `generate_secure_seed`, Haiku `random_write`). **Found on the way:**
     a self-test's loads against a held generator logged the boot's
     credit line, so the cross-boot check read a credit that never
     happened; those loads are now quiet. Tests: 2 boot tests (the seed
     file against a dropping disk and a damaged record, chaos replay from
     its seed and its interplay with an armed point), kconfig checks, a
     host test (the record length), a DevKit test, and `cxk verify-seed`
     (two boots of a copy: the second credits the first's seed and leaves
     another; red with the load removed from kmain). Untested: a disk
     error mid-rewrite (only a dropped write is), the rig's real seed,
     the chaos seed's cycle-counter fallback, a build without disk
     writes. Self-tests 196. Sabotages: `tests/sabotage/1.7c.sab`, 10 of
     10. No ABI change.
8. The hardware report: the memory map, ACPI tables and their checksums,
   MADT and APIC IDs, PCI devices, CPUID, and the clock calibrations, in a
   fixed form to serial and the USB crash log; a `cxk` command compares
   QEMU, Hyper-V and the rig. Before 9, because SMP bring-up trusts the
   MADT. Split as 1.7 was: **1.8a** the kernel builds the report, **1.8b**
   `cxk` shows and diffs reports and reads one off the rig's stick,
   **1.8c** Hyper-V's TSC calibration (59742 MHz) read through it,
   **1.8d** the rig's slow console, found on its first boot with the
   report, **1.8e** the rig's report read and diffed against QEMU's and
   Hyper-V's.
   - **1.8a (2026-11-04), done:** `@hw` lines, tab-separated, from
     `@hw begin 1` to an end line holding the count and CRC-32 of the
     lines before it (`kernel/diag/hwfmt.c`, pure); a report that runs out
     of its 64 KB says `@hw truncated`. Sections: boot, cpu and raw CPUID
     leaves, the hypervisor's leaves, the APIC state, every memory-map
     entry, the RSDP and every table the RSDT, XSDT and FADT name with its
     checksum judged, the MADT entry by entry, every PCI function with its
     BARs, and the clocks with the TSC measured again against the ACPI PM
     timer. Printed to serial before the self-tests and saved as XCRL
     record kind 6 at crash-area sector 384. A bad checksum is reported,
     not refused (Haiku `acpi_check_rsdt`, Serenity `validate_table`, not
     NT5's `ACPI_BIOS_ERROR`); deciding that waits for the rig's report.
     `verify-boot` fails a passing boot whose serial holds no whole
     report. Design: `docs/hardware/CX_HW_REPORT.md`. Tests: a boot test
     (the report checks, its sections, mem and pci lines against the
     loader and the bus, RSDP, FADT and MADT present, the crash copy reads
     back, the TSC by tick and by PM timer within 1%), 7 host tests, 2
     DevKit tests; `verify-boot`'s check red by hand (the report kept off
     serial). Untested: a bad table, a cut report and an x2APIC MADT on a
     machine (host only); a 32-bit PM timer; UEFI; the rig. Self-tests
     197. Sabotages: `tests/sabotage/1.8a.sab`, 10 of 10. No ABI change.
   - **1.8b (2026-11-04), done:** `cxk rig hwreport <disk>` reads XCRL
     kind 6 off an image or the stick (`CrashLog.ReadHwReport`); `cxk hw
     show <source>` names every field and notes what a report says is
     wrong with its own machine (bad checksums, cut, dropped, the two TSC
     rates over 1% apart); `cxk hw diff a b` compares fact by fact (a PCI
     function by address, a table by signature, a MADT CPU by UID) and
     judges each difference expected (firmware-chosen addresses, BARs with
     the same type bits, interrupt lines, the hypervisor's leaves, rates
     within 1%) or a finding; exit 1 on a finding. Design:
     `docs/hardware/CX_HW_REPORT.md` §5. Tests: 10 DevKit tests (`cxk` end
     to end among them); a booted copy's record matched its serial copy.
     Sabotages: 18 of 18, by hand (DevKit-only; the table in the commit).
     Untested: the stick itself, any report but QEMU's. Seen: TCG without
     `-icount` puts the two TSC rates 30% apart. No kernel or ABI change.
   - **1.8c (2026-11-04), done:** Hyper-V's report put the TSC at 59.8 GHz
     by the tick and 3.80 GHz by the PM timer; the tick was wrong, since
     the VM delivers about 60 of its interrupts a second for the 1000
     asked, and the TSC was measured over 50 of them. Now the TSC's rate
     is what the hypervisor states (Hyper-V's frequency MSR, where CPUID
     grants it; XNU `tsc_init`), else the PIT's count polled on channel 2
     (Haiku `calculate_cpu_conversion_factor`), else the tick as before.
     The report gains `tsc-by`, `tsc-pit-hz`, `tick-hz`, `tick-irq-hz` and
     Hyper-V's leaves to 0x40000006; `cxk hw show` notes the PIT against
     the PM timer and interrupts more than 10% off. Seen: Hyper-V's PIT
     runs 0.82-1.12% slow (stated rate, PM timer and a round 12.5 MHz APIC
     timer agree). Hyper-V now passes all 198 self-tests. Tests: the
     report test (the PIT measured, the clock not by the tick), a new
     boot test (a stated rate is the clock's), DevKit `HwDiffTests`.
     Sabotages: `tests/sabotage/1.8c.sab` 2 of 2; by hand under Hyper-V
     the stated rate ignored and channel 2's gate left shut (green under
     QEMU, which ignores the gate), both red; 4 DevKit ones by hand. A dead
     count is given up after 1000 still samples (5 million took minutes
     under Hyper-V). Untested: the two-window retry, the rig. No ABI change.
     Then the lines Hyper-V's boots lost (2-7 `@ktest` lines cut per boot,
     already at 1.8b): not the kernel's `serial_putc` but `cxk
     verify-hyperv`'s pipe reader, which started a new read after each
     500 ms quiet wait and lost what the one given up on received, the head
     of the line after each long self-test. One read is now kept
     outstanding; `verify-hyperv` passes, 198/198. Test:
     `HyperVBootTests` (a write after 1.6 s of quiet reaches the log), red
     with the old reader.
   - **1.8d (2026-11-05), done:** the rig took minutes to reach the
     prompt. Its framebuffer (the W5000's, at 0xD0000000) sits in the PCI
     hole, which its firmware's MTRRs mark uncached, and `fb_scroll_rect`
     read the console's whole viewport back over PCIe on every line, about
     a microsecond a word. Now `fb.c` draws everything into a copy of the
     screen in RAM and writes only the rectangle it changed to the screen;
     a scroll moves the copy's rows, `fb_get_pixel` reads the copy, and
     nothing reads the screen (XNU keeps a cell copy, `gc_scroll_up`;
     Serenity and Haiku move rows in the framebuffer). The screen is mapped
     write-combining: `VM_WC`, the PAT bit on PAT entry 4, which
     `pat_init` sets (the whole table written: entries 0-3 as at power-on,
     so PCD and PWT keep their meaning; entry 4 as Serenity and Haiku,
     XNU takes 6). A glyph is drawn into the copy a row at a time, not a
     call per pixel. `fb_scroll_up` (no callers) is gone. The boot log
     says the output's cost before the executive starts: on the rig 2316
     ms of drawing for 664 scrolls and 0 ms of serial waits, the whole
     boot. Tests: 2 arch tests (the PAT MSR exact; a VM_WC page is entry
     4 and holds what is written once fenced, and the screen is so
     mapped), a boot test (the screen and the copy agree after boot and
     after it; writing the screen alone shows reads and scrolls come from
     the copy; fills, glyphs and pixels reach the screen; the console's
     time counted), host tests (the PAT bit in 4 KiB and 2 MiB entries,
     VM_UNCACHED winning). Self-tests 201. Sabotages:
     `tests/sabotage/1.8d.sab`, 14 of 14 (one expectation corrected: a
     glyph left unflushed is not seen "after boot", since each log line's
     scroll rewrites the viewport). Untested: a CPU with no PAT, an AP's
     table (SMP calls `pat_init` on each), a copy that cannot be had. No
     ABI change.
   - **1.8e (2026-11-05), done:** the rig's report (`rig hwreport`) diffed
     against QEMU's default CPU, `--cpu Opteron_G5` and Hyper-V's: 145,
     140 and 133 findings, most of them a different machine's PCI
     functions, CPUs and ACPI firmware names. Written up in
     `hardware/CX_HW_REPORT.md` §7. For step 9: the rig's APIC IDs are
     `0x10`-`0x17` (16 more entries disabled), the I/O APIC is `0x18`, and
     CPUID leaf 1 says the BSP is 0 when it is `0x10`, so APs are started
     by the MADT's IDs and none is assumed. IRQ 9 is level, active low,
     which `route_irq` honours. ACPI 1.0 (RSDT only), every checksum ok,
     no IVRS. The TSC is invariant, 4.000 GHz by the PIT and the 32-bit PM
     timer alike. Nothing in the kernel was wrong for it. `hw diff` fixed:
     bare metal's `hypervisor -` made the VM's max-leaf a finding;
     `HwDiffTests` covers it, seen red first. Untested: the rig's AHCI,
     OHCI, EHCI and RTL8168 parts under their drivers. No ABI change.
9. SMP readiness: a lock order table and a checker in debug builds,
   per-CPU data, spinlocks that save interrupt state, the slab allocator.
   Physical memory is reached only through the direct map's helpers, never
   by adding `DIRECT_MAP_BASE` by hand, so a 32-bit port can swap in
   temporary mappings. Then SMP on 8 cores, then the race tests of 6 as
   the SMP stress suite (`-smp 8` under QEMU's multi-threaded TCG, Hyper-V
   for real parallelism). Spinlocks already save the interrupt state
   (`sync/spinlock.h`, since step 0) and `ocache` is already a slab
   allocator, so those two become a check that they are used everywhere
   rather than new code. Split as 1.7 and 1.8 were:
   **1.9a** the lock order: every spinlock given a rank from one table,
   a checker in test-hook builds that panics on a lock taken out of order
   (Serenity `Kernel/Locking/LockRank.h`, WRK's numbered queued locks in
   `ntkeapi.h`), the order read off a boot rather than guessed, and the
   heap's large-block path, which takes no lock today, given one;
   **1.9b** per-CPU data (GS base, `swapgs` on entry, the current thread,
   the syscall stack, `spin_held` and the checker's held list per CPU) and
   the scheduler's interrupts-off sections turned into a sched lock placed
   in the order; **1.9c** physical memory only through the direct map's
   helpers, and every allocation path on `ocache` or the locked heap;
   **1.9d** the APs started by the MADT's APIC IDs (never CPUID's), each
   with its GDT, TSS, IDT, `pat_init` and LAPIC timer, parked idle;
   **1.9e** the scheduler on every CPU: run queues, IPIs, TLB shootdown;
   **1.9f** the race tests of 6 as the SMP stress suite (`-smp 8` under
   multi-threaded TCG, Hyper-V), then the rig.
   - **1.9f plan** (2026-11-07, Aurora agreed): the stress suite is also
     the profile 1.9e2's run-order choice waits on (`kernel/CX_SMP.md`
     §4a, §6). Test builds count `sched_lock` contention: acquisitions,
     acquisitions that had to spin, spin iterations, and the longest hold
     in TSC ticks, per CPU, printed as one fixed line at the end of the
     suite (and in the hardware report's form, so `cxk hw diff` can set an
     `-smp 8` QEMU run beside Hyper-V and the rig). The count's own test:
     two CPUs forced through one contended acquisition must report a spin,
     and a sabotage that drops the count goes red. **The decision rule:**
     per-CPU run queues and a balancer are planned (a step of their own,
     Haiku's `scheduler_thread.h` and WRK's per-processor ready lists the
     references) only if the suite on 8 CPUs, under Hyper-V or on the rig,
     shows contention that costs; otherwise the one order stays and
     §4a says so with the numbers. TCG's figures alone decide nothing:
     its CPUs are host threads and its timing is not the hardware's.
     **Alongside:** the first host fuzz target (`safety/CX_SAFETY.md`
     §3.2, §7): `cxfs_sb_validate` and the mount path that trusts what it
     passes, built for the host under libFuzzer, ASan and UBSan with
     `kernel/test/host`'s seam; a corpus in the tree seeded from real
     images; every crash a regression test in the host suite. The
     target's own check: a sabotage that removes one of
     `cxfs_sb_validate`'s bounds is found by the fuzzer within a stated
     budget (the figure set when it is first measured).
   - **1.9f (2026-11-07), done except the rig:**
     test builds count `sched_lock`'s contention per CPU
     (`sync/lockstat.h`, `spin_lock_counted`), proved by a self-test that
     holds the lock while AP 1 takes it; the stress suite
     (`kernel/ktest_stress.c`: a token ring over one thread per CPU, then
     a yield storm) runs last and prints the profile in the hardware
     report's form (`cxk hw show/diff --profile`); `cxk verify-boot
     --stress` boots it on 8 CPUs under multi-threaded TCG, without icount,
     and does not judge the two tests that time against icount's clock
     (`BootVerifier.StressNotJudged`). **Hyper-V's APs started for the
     first time:** the trampoline's 64-bit segment was loaded unaccessed
     from a read-only page, a triple fault TCG never showed; every
     descriptor is now accessed, and a self-test holds it. The modexp
     shootdown test repeats until its 300 ms pass. **The decision:** on
     Hyper-V, 8 vCPUs, the whole suite spun 274 ms on `sched_lock` (9.6 %
     of 775 087 takes contended), the stress test 19 % of its CPUs' time
     with nothing else to run: **the one order stays** (`kernel/CX_SMP.md`
     §4d), until the rig's figure or a workload with work says otherwise.
     240 self-tests (Hyper-V 2 and 8 vCPUs, QEMU 4 and 1, all pass; the
     stress boot 239 judged). Sabotages: `tests/sabotage/1.9f.sab`. No ABI
     change. Open: the rig's profile (no way off a serial-less machine yet,
     `hardware/CX_HW_REPORT.md` §9).
     **The CXFS fuzz target, done** (`safety/CX_SAFETY.md` §3.2.1):
     `kernel/test/host/fuzz_cxfs.c` runs the kernel's `cxfs.c` under
     libFuzzer, ASan and UBSan over RAM disks (`hostdisk.c`), each input a
     hostile device mounted as the root and as a second volume, with an
     oracle for every block touched without reason; corpus
     `kernel/test/host/corpus/cxfs`, seeded from the volume a built image's
     first boot installs, replayed by the host suite (130 tests); a fixed
     20 000-input run in `dotnet test`. Found nothing in CXFS in 314 324
     inputs. The bound sabotage (`manifest_count` against
     `manifest_blocks`) was found by 10 seeds of 10 from the real seeds,
     within 396 to 135 689 inputs: **the budget is 300 000 inputs**. By hand,
     since `cxk test sabotage` judges boots only.
   - **1.9a (2026-11-05), done:** every spinlock carries a rank
     (`kernel/sync/lockorder.h`, 14 classes), given where it is made
     (`SPINLOCK_INIT(LR_x)`, `spin_init`); with `CXK_LOCK_CHECK` (on with
     the test hooks) `spin_lock` checks before it spins that every lock
     held ranks lower, and a lock taken out of order, two of one rank, an
     unranked lock, more than 8 held, a release not held or a thread
     switch holding one panics naming each lock and where it was taken.
     The order was read off a boot (a build that recorded every held/taken
     pair with no check), not guessed: two guesses were wrong (`pids` under
     `vmregion`, the cache registry under a cache). Each boot prints the
     pairs it saw (`[LOCKS]`). The heap's large path (over 2 KiB: the
     first-fit list and `heap_grow`) took no lock at all; it has
     `heap_lock` now. Design: `docs/kernel/CX_LOCK_ORDER.md` (Serenity
     `LockRank.h`, WRK `KSPIN_LOCK_QUEUE_NUMBER`, XNU `add_held_rwlock`).
     Not yet in the order: the scheduler's interrupts-off sections (1.9b).
     Tests: 6 host tests, each seen red under hand sabotage (8, in the
     commit); a boot test (in order, each failure caught with the checker
     armed, and the heap's alloc, free and count each shown taking the
     heap lock); every test-hook boot is the wide test, and passed with
     the executive and the shell. Hand sabotages of the table out of step
     with a real nesting and of an unranked lock each panic the boot with
     the checker's message. Untested: paths no boot reaches. Self-tests
     202. Sabotages: `tests/sabotage/1.9a.sab`, 9 of 9. No ABI change.
   - **1.9b (2026-11-05), done:** each CPU's own data is a `struct cpu`
     (`kernel/cpu/percpu.h`: self, the syscall stack, the parked user RSP,
     the running thread, `spin_held`, the checker's held list), found
     through the GS base, which holds `cpu_boot` from before `kmain`
     (WRK `KPCR` Self, XNU `cpu_data` `cpu_this`). Ring 3's base is
     always 0 (no FSGSBASE, so nothing per thread to keep): `swapgs` is the
     syscall entry's first instruction and comes before `sysret` and
     `enter_usermode`'s `iretq`; `isr_common` swaps when the frame's CS is
     ring 3. #DB, NMI, #DF and #MC, which can land anywhere (the syscall
     entry's first instruction included), read GS_BASE and swap only if
     its top bit is clear, as XNU's `idt64_debug` does. The syscall entry's
     stack and the parked RSP moved off the TSS and a global into the
     struct. The scheduler's interrupts-off sections are `sched_lock`
     (`LR_SCHED`, after `kwait`), held across `context_switch` and given
     back by the thread switched to (or `thread_launch`, which then
     enables interrupts), as Serenity's `g_scheduler_lock` is; `switch_to`
     asserts it, and only it, is held. The reaper claims an exited thread
     under the lock (`THREAD_REAPING`) and frees it outside, idling gives
     the lock back before the halt, and the timer never calls in holding
     it. Design: `docs/kernel/CX_LOCK_ORDER.md` §2a. Tests: a boot test
     that nine scheduler entries take the lock and a new thread and the
     one switched back to find nothing held; an arch test of GS, the
     running thread and the syscall stack across a switch, and of the IST
     vectors' swap with ring 3's base in and without. Hand sabotages (each
     panics every boot): a thread launched holding the lock, a switch
     without it, the `swapgs` dropped from the syscall exit and from the
     interrupt exit. Untested: `sched_yield_if_outranked` and
     `thread_block_prepare_until` taking the lock; the early releases
     (one CPU cannot contend). Left for 1.9e: lockless per-thread reads,
     a reaper that checks a thread is off every CPU, IPIs to end a halt.
     Self-tests 204. Sabotages: `tests/sabotage/1.9b.sab`, 12 of 12. No
     ABI change.
   - **1.9c (2026-11-06), done:** a frame is reached by its physical
     address only through `kernel/memman/physmap.h`: `phys_map` /
     `phys_unmap` (per CPU, nested last in first out up to 8, interrupts
     off between), `phys_zero`, and `phys_map_lasting` for the page
     database; the arch gives per-CPU slots, the direct map on x86-64.
     An unmap out of order, an unmap of nothing, a ninth mapping, a frame
     past `RAM_REACH` and a thread switch with one open each panic
     (Serenity `quickmap_page`, Haiku `GetPageCurrentCPU`, WRK
     `MiMapPageInHyperSpace`, XNU `PHYSMAP_PTOV_check`). The build refuses
     the direct map's names outside the arch and `physmap.c`. Every
     `paging_temp_map` user converted, and the ones that never unmapped
     (the system page, the kernel's CXEX loader, through a new `put_page`
     op) now do. Test-hook builds fill a freed frame with `PMM_POISON`;
     the first boot with it found new processes' user stacks handed out
     unzeroed (`spawn.c`), now cleared. Allocation paths: the kernel
     stacks' slots took no lock (now `LR_KSTACK`), file locks were a
     static array (now an `ocache`, capped at 64). Design:
     `docs/kernel/CX_PHYS_MEMORY.md`. Tests: a boot test of the checker
     (nesting, the offset, interrupts, each failure counted with it
     armed, `phys_zero`, poisoning, a refused free left alone); a boot
     test that a kernel stack's alloc and free each take its lock twice;
     four file-lock steps (one object per lock, an allocation failure
     refused cleanly, the cap, given back); an X test that a new
     process's stack is zeroed. Hand sabotage: the loader not giving back
     its page panics every boot (the switch check). Untested: a refused
     free poisoned (its sabotage overwrites kernel text); the panic
     path's unchecked mapping. Left for 1.9e: the file system layer and
     `sysfile`'s graveyard and waiters, which take no lock. Self-tests
     207. Sabotages: `tests/sabotage/1.9c.sab`, 12 of 12. No ABI change.
   - **1.9d (2026-11-06), done:** every CPU the MADT lists as enabled is
     started by the APIC ID it lists (`kernel/arch/x86/madt.c`, pure and
     host-tested: disabled, duplicate and unreachable entries dropped),
     one at a time: INIT, STARTUP, a second STARTUP only if needed, a
     second to park, INIT again and its stacks freed if it never does
     (Haiku `smp_boot_other_cpus`, XNU `intel_startCPU`; not Serenity's
     broadcast). A trampoline at 0x8000 (`smp_tramp.asm`) goes real →
     protected → long mode on a root of its own below 4 GiB
     (`paging_boot_root`), the boot CPU's CR0 and CR4. Each AP: its own
     GDT and TSS (`gdt64_init_cpu`, a table per CPU), `struct cpu` behind
     GS (a table of 16), RSP0 and IST stacks, the one IDT, `pat_init`,
     the syscall MSRs, its local APIC with the timer masked; then parked
     in `sti; hlt`, woken only by `smp_call`'s IPI (vector 57). An AP's
     interrupts are counted and acknowledged, nothing more. The
     verification boot gives QEMU four CPUs at IDs 0, 1, 2, 4 and two
     listed disabled (`BootVerifier.X86_64Smp`; `verify-boot --smp`).
     Design: `docs/kernel/CX_SMP.md`. Tests: four host tests of the walk
     (the rig's shape, QEMU's gaps, x2APIC and duplicates, the cap and
     damage); boot tests that every enabled CPU runs by its own ID, that
     each AP's tables, stacks, PAT, MSRs, control registers and local
     APIC are as set (read on the AP), that its own timer interrupts it
     twice, acknowledged, and that a parked AP halts and one call wakes
     it once. Hand sabotages: the walk's enabled and duplicate filters
     (host tests red); an AP without `cpu_local_bind` triple-faults, which
     resets the whole machine every boot. Untested: a CPU that never parks
     (QEMU starts every CPU it lists); the steps that an AP answers to its
     own ID and shares the one IDT have no sabotage short of a reset.
     Left for 1.9e: threads on the APs, IPIs between running CPUs, TLB
     shootdown, the TSC's agreement. Self-tests 211. Sabotages:
     `tests/sabotage/1.9d.sab`, 14 of 14. No ABI change.
   - **1.9e split** (2026-11-06), as 1.7 and 1.8 were: **1.9e1** the IPIs
     between CPUs and the TLB shootdown, which must exist before a second
     CPU runs anything that maps; **1.9e2** the scheduler on every CPU
     (per-CPU run queues, an idle thread per CPU, the IPI that ends a
     halt for a wake, a reaper that checks a thread is off every CPU, the
     lockless per-thread reads 1.9b left); **1.9e3** what 1.9c left unlocked
     (the file system layer, `sysfile`'s graveyard and waiters) and the
     TSC's agreement between CPUs (itself split: 1.9e4 below).
   - **1.9e1 (2026-11-06), done:** every change to a present entry goes
     through one funnel (`entry_changed`, `paging.c`): a local `invlpg`,
     then `smp_tlb_shootdown` (`arch/x86_64/smp.c`). A kernel-half change
     goes to every CPU; a user one only to CPUs whose recorded CR3 is that
     space (`space`, stored before the load with a fence between; the
     sender fences after the write and reads it, a Dekker pair). An
     idle parked CPU is marked stale instead of interrupted and reloads
     CR3 as it wakes (XNU's idle CPUs). One request per sender in its own
     `struct cpu`, a mask of targets still to answer (WRK
     `KiIpiSendRequest`), on vector 57, served by every CPU, the boot CPU
     included. No deadlock: a sender serves while it waits (Serenity
     `smp_broadcast_wait_sync`) and a lock's spin serves too (Haiku
     `acquire_spinlock`). A CPU that never answers is withdrawn after
     10 s and the paging layer panics. `paging_unmap` returns the frame,
     freed after the shootdown; `kstack`, `fb` and `ocache` freed before
     (fixed). Design: `docs/kernel/CX_SMP.md` §4. Tests: a kernel page
     changed under a busy AP is read new there; an idle AP is marked,
     not woken, and flushes on waking; a user space's change reaches the
     CPU running it and never the one outside; two APs shooting at each
     other with interrupts off, and a CPU spinning on a lock the sender
     holds, all finish. Hand sabotage: an AP's IPI that serves nothing
     ends the boot in the panic ("TLB shootdown: a CPU never answered").
     Untested: the fence pairs, `icr_send`'s interrupts-off section, the
     withdrawal after a timeout. Self-tests 215 (4 CPUs, 1 CPU,
     `--cpu max`). Sabotages: `tests/sabotage/1.9e1.sab`, 9 of 9. No ABI
     change.
   - **1.9e2 (2026-11-06), done:** the scheduler on every CPU, under the
     one `sched_lock`. One order rather than a run queue per CPU
     (Serenity's `g_ready_queues`; per-CPU queues and a balancer wait for
     1.9f's profile): `next_runnable` picks the highest thread whose
     `affinity` has this CPU and that is on no other (`on_cpu`). An idle
     thread per CPU (WRK's `Prcb->IdleThread`), outside the id table and
     the count, halts with interrupts off from the look on: no thread
     halts on its own stack any more. A thread made ready or raised is
     handed an idle CPU, or the one running the lowest thread below it,
     by `need_resched` and one IPI per ask (Haiku's `enqueue`); one
     switched out still ready is offered the same way. Threads are made
     for the boot CPU until 1.9e3 locks the file system layer;
     `thread_create_on` and `thread_set_affinity` place one elsewhere.
     The reaper frees only a thread on no CPU, on the boot CPU. A
     deadline made on an AP is forwarded to the boot CPU's event timer;
     an AP's slice is its own local timer (none under the PIT). A thread
     finds itself through `struct cpu`'s `cur`; another's record is read
     under the lock (`look`). The interrupts-off bounded wait and
     `timer_sleep`'s busy loop serve shootdowns, which settles 1.9e1's
     open interrupts-off case. Design: `docs/kernel/CX_SMP.md` §4a,
     `CX_LOCK_ORDER.md` §2a. Tests: each CPU's idle thread; placement on
     the boot CPU or the AP asked for; a wake from the boot CPU ends an
     idle AP's halt by IPI; a sleep on an AP ends at its time; an AP's
     timer ends a slice; a raised thread takes a busy AP at once; an
     affinity change moves a running thread; an interrupts-off poll
     answers an AP's shootdown; the getters on another thread take the
     lock; a thread two CPUs may run, woken while still on its CPU, is not
     taken by the other. Hand sabotages: `runnable_here` without its
     `on_cpu` check panics in that last test (green before it existed);
     every thread made for any CPU reddens placement and some 25 tests of
     the subsystems 1.9e3 locks; the reaper's `on_cpu` check is covered
     by the handoff. Self-tests 224
     (4 CPUs, 1 CPU, `--cpu max`). Sabotages: `tests/sabotage/1.9e2.sab`,
     8 of 8. No ABI change.
   - **1.9e3 split** (2026-11-06): run with a process's threads free to
     move and the reaper on every CPU, the boot failed 11 tests, and the
     cause under most of them was not a lock: a syscall checks a user
     range and then reads it directly (148 places), and a sibling thread
     on another CPU can unmap it between the two. **1.9e3** keeps the
     locks, the reaper on every CPU and the TSC's agreement; **1.9e4**
     moves every user read and write to a recoverable copy, then lets a
     process's threads leave the boot CPU, with those 11 tests as its
     list (most assume one CPU's ordering: yield loops, a helper on the
     same CPU).
   - **1.9e3 (2026-11-06), done:** the storage lock (`disk.h`, first in the
     order, CPU-recursive by `sync/rspin.h`) over every CXFS entry point,
     disk transfer, crash-log save and file syscall; `sysfile`'s
     `files_lock` over the graveyard, the advisory locks and their waiters
     (blocking as `kobj_wait` does) and unlink's open check, read with the
     last close's under it now; the log lock (`console.h`, last,
     CPU-recursive) over the console, its serial tee and the ring, with
     `SYS_LOG_READ` and `console_write` copying outside it; the CMOS lock;
     both recursive locks broken by a panic. The reaper on every CPU. The
     TSC's agreement: each AP measured against the boot CPU by 64 round
     trips as it parks (XNU's two-sided meeting, its 4096-cycle margin)
     and set right by a per-CPU offset the clock takes off (not Haiku's
     TSC write, which QEMU's TCG ignored). Tests (`ktest_arch.c`): each
     lock held on the boot CPU keeps an AP's thread in its call and lets
     it through after; the ring whole under two CPUs; the APs reap with
     the boot CPU spinning; an AP's TSC put out both ways is found and set
     right, and its clock moves with its offset. Self-tests 230
     (4 CPUs, 1 CPU, `--cpu max`). Sabotages: `tests/sabotage/1.9e3.sab`,
     7 of 7. No ABI change.
   - **1.9e4 split** (2026-11-06): of the 148 direct reads, about 30 were
     syscalls' and the rest the tests' own setup; but with the copies in,
     the threads still could not leave the boot CPU, because much of the
     kernel is still kept to one CPU by turning interrupts off: the
     process table's counts and stop list (`proc.c`), the jobs (`job.c`),
     the keyboard's buffer and its one waiter, the framebuffer against the
     console, a background process's output, and the network stack, which
     yields while it polls. **1.9e4a** is the copies and a build rule that
     keeps them; **1.9e4b** locks those, then lets the threads move, with
     the 11 tests as its list.
   - **1.9e4a (2026-11-06), done:** no syscall reads or writes user memory
     where it lies. Every one copies through `user_copy_in`/`out`, a
     string through `user_copy_str` (a page at a time, so a string ending
     before an unmapped page is whole) and the futex's word through
     `user_fetch_u32`; file and network data cross through a kernel bounce
     buffer, and an argument struct is read once and used from the copy
     (vm_map, spawn's arguments and blob, exec's). `user_ptr` is private to
     `usercopy.c`; the tests reach their own user pages through
     `diag/ktest_umem.h`, which `check_arch_boundary.cmake` allows only in
     the tests (a planted call in `futex.c` stops the build). Test
     (`usermode_copies_test`): each converted call is handed a mapped page
     that `ucopy_gone` (`testhook.h`) makes every copy fail on, as the
     hardware's recovery does once the page is unmapped after the check,
     and must refuse it, through the copy, leaving the page as it was.
     Self-tests 231 (4 CPUs, 1 CPU, `--cpu max`). Sabotages:
     `tests/sabotage/1.9e4a.sab`, 13 of 13 (and two by hand, each a panic
     in an earlier test). No ABI change.
   - **1.9e4b (2026-11-07), done:** a thread made to run ring-3 code runs
     on any CPU; kernel threads still start on the boot CPU
     (`THREAD_ON_BOOT_CPU`, not audited). Locked: the processes' counts,
     end and stops (`proc_lock`, the last thread out decided in one hold),
     the job tree (`jobs_lock`), the keyboard buffer and waiter
     (`kbd_lock`), a process's output, the framebuffer syscalls and font
     install (log lock), a font's selection (storage lock), and the
     `@ktest`/`@hw` serial lines (log lock). The network and power calls
     move their caller to the boot CPU for the call (`ON_BOOT_CPU`,
     transitional). A signature check serves shootdowns
     (`bn_mod` → `smp_serve`). The tests that assumed one CPU wait for the
     reap (`proc_live_settles`, `ir_gone` on ENDED). Six new SMP
     self-tests (`CX_SMP.md` §4c). Self-tests 237 (4 CPUs, 1 CPU,
     `--cpu max`). Sabotages: `tests/sabotage/1.9e4b.sab`, 11 of 11. No
     ABI change.
10. A documentation pass: the README and `safety/CX_SAFETY.md` §1 brought
    up to the branch as it stands.
   - **1.10 (2026-11-07), done:** the README's status says what Phases 0
     and 1 built and what is not yet there, and its command table gains
     `--smp 1`, `verify-hyperv`, `verify-seed`, `hw show`/`diff`,
     `test all` and `test sabotage`; CX_SAFETY §1 gives today's counts
     (237 self-tests, 23 ring-3 tests, 392 DevKit, 127 kernel-host,
     13 Differential, 21 sabotage files), the hooks and runtime checks,
     and says what is still none: fuzzing, property tests, model checking,
     proofs, coverage, mutation testing. No code change.

Alongside, one item per step where it fits (`safety/CX_SAFETY.md` §7):
host fuzz targets (CXFS first, then CXEX, CXBI and packets), the ABI
layout cross-check, random X programs against both compilers, and the X
language work in `language/CX_X_PLAN.md`.

### Phase 2: the GUI foundation

`system/CX_DISPLAY.md`, `kernel/CX_DRIVERS.md`.

1. Device tree and scored matching; today's drivers converted with no change
   in behaviour.
   - **2.1 (2026-11-07), done:** `drivers/devtree.{h,c}` (`CX_DRIVERS.md`
     §3.1): one tree of the root, the platform bus (the legacy ATA
     channels), every PCI function and every USB device under its
     controller. Drivers declare a match table and `DRIVER_DECLARE`
     themselves into `.cxk_drivers`; a match scores by the fields it
     names (device > vendor > interface > subclass > class), a declined
     start falls to the next-best, and drivers bind in groups at the
     boot's old points, by `order` within a group, under a group limit
     (netif: one NIC). All nine drivers converted: e1000, rtl8169, ATA,
     AHCI, xHCI, EHCI, OHCI, USB HID, USB storage; each singleton keeps
     its one try, so behaviour is unchanged. Lock `LR_DEVTREE` (a leaf).
     Five self-tests (250). Alongside, the Phase 1 review's first fix:
     `user_fetch_u32` checks the page (passively, under the futex lock)
     before the copy. Sabotages: `tests/sabotage/2.1.sab`.
2. Driver-host primitives: register ranges as memory objects, interrupts as
   handles, DMA buffers. **Done (2.2, ABI 33):** `kernel/drivers/devhost.h`,
   `SYS_DEVICE_INFO`/`OPEN`/`BAR`/`IRQ`, `SYS_IRQ_ACK`, `SYS_DMA_ALLOC`
   (`kernel/CX_DRIVERS.md` §4 step 3). `GRANT_IOPORT` became
   `GRANT_DRIVER`, in no baseline (no IOMMU: full trust); it gates the
   open, the handle's rights the rest. A device is `DEV_HELD` while any BAR
   object, its interrupt or a DMA buffer refers to it; BARs map uncached and
   never executable, whole pages only; MSI only; a DMA buffer let go while
   the device is open waits for bus mastering to go off before its pages
   are freed. PCI configuration access is now under `LR_PCI`; the MSI
   vector pool is lock-free and an MSI ends with a preemption point. Tested
   on QEMU's `edu`, which verify-boot attaches. Alongside, the review's
   second fix: `futex_wait` keys the word again after its locked read and
   starts over if a remap moved it (`ktest_futex_remapped`). Four
   self-tests. Sabotages: `tests/sabotage/2.2.sab`.
3. The display server: back buffer, damage, virtual screen for CI.
   **Done (2.3, ABI 34):** `SYS_SCREEN_OPEN` (0x47, `GRANT_FRAMEBUFFER`)
   gives the screen to one holder as a memory object (`KMEM_SCREEN`,
   mapped write-combining, never executable); the screen's last reference
   gives it back and the kernel repaints its console from its copy. While
   it is held, the console and the `SYS_FB_*` calls draw into the copy
   only; `panic_begin` takes the screen back for good
   (`fb_panic_takeover`; that the panic path calls it is not tested).
   `os/std/display.xfxn`: damage (clipped, merged, one bounding box past
   eight), premultiplied source-over, layers composed from the topmost one
   that hides the rectangle, a virtual screen (XRGB8888 or RGB565) that
   frames are checked on: spot pixels worked out by hand and the frame's
   FNV-1a fingerprint. `os/apps/displayd.xfxn` is the server's first form
   (`run +screen displayd`; Esc gives the screen back). Windows and input
   as handles are 2.4. Six self-tests. Sabotages: `tests/sabotage/2.3.sab`.
   **3a. The panic screen** (Aurora, 2026-11-08: sooner rather than later;
   after the 2.3 review's shootdown fix, before 2.4). It replaces
   x86-64's minimal `kpanic` (`TRANSITIONAL_CODE.md`):
   - A plain-language headline and advice, a stop code (a name such as
     `KERNEL_PAGE_FAULT` and a short code), the crash number, and a short
     symbolised trace. The full record (trace, registers, log tail) goes
     to the crash area as before, which makes `verify-crashlog` pass on
     x86-64 again (the record has no "Stack trace" today).
   - **A QR code holding the essentials as plain ASCII** (byte mode, error
     correction M): stop code, crash ID, version and ABI, the reason,
     CPU/thread/pid, `rip` and the key registers, a trace of about five
     frames, the log's last lines. About 0.7–1.2 KB, so one code (version
     25 or below). The log stays in the crash area for `cxk rig bootlog`.
   - **Drawn only when the screen has room:** `room = min(right column,
     height left)`, `px = min(8, room / (modules + 8))` with a 4-module
     quiet zone; the code is drawn only at `px >= 3`. Otherwise the text
     layout stands alone and its advice says to note the stop code and
     crash number. 1024×768 and up show it; 640×480 does not.
   - The encoder is Nayuki's `qrcodegen` (MIT), vendored unmodified under
     `vendor/`, with static buffers (a panic may run on the double-fault
     stack). Check the licence and add the `THIRD-PARTY-NOTICES.md` row.
   - It draws over the display server: the panic path's `panic_begin`
     call is tested here at last.
   - Tests: the fit rule's edges (exactly 3 px fits, 2 does not); a ktest
     encoding a fixed record against a fingerprint; a host test decoding
     it (ZXing.Net, Apache-2.0) so a sabotage proves the code readable;
     a forced panic in QEMU, screendumped and decoded on the host.

   **Done (3a):** `kernel/diag/panicrep.c` writes the report and the code's
   text and encodes it, portable and tested on the host;
   `panicscreen.c` draws it; `ksym.c` names addresses from a table
   `cxk ksyms` generates in a two-pass link (the table is rodata after
   `.text`, so no function moves). The kernel keeps frame pointers now, and
   `panic_stack_walk` checks every frame is mapped before reading it. A
   panic owns the machine through one gate: the first CPU in wins, the
   others stop on an IPI (`smp_panic_stop`), and a panic during the panic
   only reports on COM1 and halts. `verify-crashlog` passes on x86-64
   again. `cxk verify-crashlog --screendump` writes the screen through
   QEMU's QMP, and `PanicScreenTests` reads it with ZXing.Net while
   another CPU paints over the screen and it is held as the display server
   holds it. `2.3a.sab` 10 of 10; by hand, judged by the decoder or the
   boot: no takeover, no CPU stop, no stop check, an unreadable code, an
   unmapped frame, a symbol reader without its range check. Untested: the
   nested-panic path (`panic_again`), a 640×480 boot end to end (the fit
   rule is tested, its caller on that screen is not), a CPU that cannot
   take the IPI, a panic in text mode, the USB variants.
4. Windows, transparency, server-drawn frames, focus and input routing.
   **Done (2.4, ABI 35):** the keyboard and pointer as one exclusive,
   non-transferable handle (`SYS_INPUT_OPEN`, `SYS_INPUT_EVENTS`); a lease that
   keeps a lent buffer unwritable until it ends (`SYS_MEM_LEND`,
   `SYS_MEM_LEASE_CHECK`); `os/std/wm.xfxn`, `dserver.xfxn`, `window.xfxn`;
   `displayd` rewritten and `wdemo` as a client. Fifteen self-tests, frames
   checked on a virtual screen, two real client programs driven through the
   protocol, a hostile one refused. Sabotages: `tests/sabotage/2.4.sab`.
   `run` has grants `+spawn` and `+endpoint`. Not tested: on real hardware or
   by hand on a screen, a lease on non-anonymous memory (no user handle can
   ask), a client dying with a lease in flight beyond the close path, USB input
   through the handle. Starting a client blocks the server's loop for as long
   as the signature check takes.
5. Retained layers and spring animations in the compositor. **Done (2.5)**
   (`os/std/scene.xfxn`, `CX_DISPLAY.md` §6, `2.5.sab` 42 of 42). Not tested:
   any of it by hand on a screen, its frame rate on real hardware (each scaled
   or faded pixel is X code), several monitors (there is one screen).
6. Trusted UI: the uninterceptible key combination, consent screens, signer
   in title bars, capabilities for capture and overlays. **Done (2.6, ABI 36)**
   (`CX_DISPLAY.md` §7, `2.6.sab` 49 of 49). Ctrl+Alt+Del pressed on PS/2 and
   USB keyboards under QEMU, seen on the screen, by hand. Not tested: any of it
   on the rig or by a person; the PS/2 and USB callers have no boot test (the
   decoders have host tests); a `[Publisher]` signer from a real vault key
   (only formatted from a value); the consent screen's text. Not built: global
   hotkeys and full screen (nothing to guard yet), publishers' names.
7. Bochs display, then virtio-gpu 2D. The Process Manager.
   **2.7a, the Bochs display: done (ABI 37)** (`CX_DISPLAY.md` section 8,
   `2.7a.sab`): the first driver in a process, `os/std/bochsdrv.xfxn`, run
   as `run +driver bochs [<width> <height> [16|32]]`, and `SYS_SCREEN_MODE`,
   by which it tells the kernel the screen's new geometry. The executive
   holds `GRANT_DRIVER` and gives it to the shell alone. Not tested: a device
   that does not take a mode (a page of RAM always reads back what was
   written), a mode change racing a panic, any of it on real hardware (the
   rig has no Bochs display). Not built: a change while `displayd` holds the
   screen, EDID, X's `*mmio` type (the registers go through
   `os/std/mmio.xfxn`).
   **2.7b, virtio-gpu 2D: done (ABI 38)** (`CX_DISPLAY.md` section 8,
   `2.7b.sab`): `os/std/virtio.xfxn` (the transport, polled) and
   `os/std/virtiogpu.xfxn`, `SYS_DEVICE_CONFIG` (0xF7, read only), `run +driver
   virtgpu`, and `displayd --virtio-gpu`, whose present sends each damaged
   rectangle. The image's `memory_quota_mib` is 64 since this step. A panic does not show on this display (the kernel cannot draw
   there): decision in `CX_DISPLAY.md`. Not tested: any of it on real hardware;
   the picture itself except by a screendump by hand.
   **2.7c, the Process Manager on the console: done (ABI 39)** (its
   performance tab, sensors and system info: `system/CX_SYSTEM_MONITOR.md`)
   (`kernel/CX_SYSCALLS_IPC.md`, "What is running, to read"; `2.7c.sab`):
   `SYS_PROCESS_QUERY` (0x78) and `GRANT_INSPECT`, which also reads the
   device tree; a process has a name (its file's), the pid that started it
   and the CPU time of all its threads; `os/std/sysview.xfxn` and
   `run +inspect procman [watch [n] | devices]`. It only reads. Not built:
   a window under `displayd` (the list is on the console), ending a process
   from it (no handle comes of the query: whose authority that is has not
   been decided), per-driver interrupt rate, DMA use and restart count
   (`CX_DRIVERS.md` section 3, item 8), idle time and per-CPU load, threads within
   a process.
   **The Phase 2 review** (`docs/reviews/2026-11-11/PHASE2_REVIEW.md`, local):
   findings 1 and 2 fixed (ABI 40, `review-2.sab` 7 of 7): `SYS_DEVICE_OPEN`
   refuses a bridge (class 0x06, or any header but type 0), `pci_bar_get`
   reads only the header type's BARs, `SYS_DEVICE_BAR` refuses base 0, and
   `SYS_INPUT_OPEN` needs `GRANT_FRAMEBUFFER` beside `GRANT_INPUT`. Not taken:
   restoring a firmware-set Bus Master bit on close, since the departed
   driver may have left the device aimed at the DMA pages freed then.
   Finding 3 was already fixed in 2.7c's commit (both sabotages re-run, red).
   Open, the lows: command-register writes clear the write-1-to-clear status
   bits (`pci.c`, `devhost.c`); the screen's address held in 32 bits
   (`devhost.c`); the HID report descriptor's 256-byte buffer and `uint16_t`
   bit offsets; consent answered by a text key; `CX_SMP.md` §4a's lost-kick
   bound understated; 2.7c's `parent` a reusable pid; a thread's last slice
   not counted at exit. Untested: type 1's and 2's BAR limits past BAR 0
   (edu has one BAR).
8. **The toolkit, a terminal, and the GUI as the default session** (Aurora,
   2026-11-11: taken before Phase 3, the one exception to "keep in order").
   Base apps are built on a shared toolkit, not each drawing pixels the way
   `wdemo` does. Survey Haiku's Interface Kit (`BWindow`/`BView`,
   `src/kits/interface`), SerenityOS LibGUI (`Widget`, layouts) and NT's
   user32 window classes before 2.8a's design; cite by file.
   - **2.8a, the client toolkit** (`os/std`, over `window.xfxn` and
     `display.xfxn`): an event loop over `win_wait`, damage collected per
     widget and committed as rectangles, one theme (colours, metrics, the
     XFNT font), text clipped to its box, and the first widgets: label,
     button, text field, list, scroll view, and a fixed-cell text grid (the
     terminal's). **The base set is built before the terminal** (Aurora,
     2026-11-11, replacing "only as far as its first programs need"), so no
     app draws its own controls: label, button, text field (with selection,
     the clipboard and the input-method interface), check box, radio, switch,
     slider, progress bar, list, tree, table, tabs, scroll view, menu and
     context menu, tooltip, the dialog with its standard buttons, and layout
     containers (rows, columns, grid, spacing from the design system); the
     text grid with them. **A widget gallery app** shows every widget in
     every state, light and dark, at 100, 150 and 200% scale and in the
     pseudo-locale: it is the step's witness, the automation checks
     (goal 9) run over it in the verification boots, and each is sabotaged.
     Serenity's WidgetGallery and GTK's widget-factory are the precedent.
     Leaning (Aurora agreed, 2026-11-11; the survey may overturn it):
     **retained** widgets (state in the widget, damage per widget, a tree
     for focus and later accessibility), a fixed set of kinds dispatched by
     a switch plus a draw callback for custom ones, no inheritance until
     something needs it. Text goes through a font handle and a size from
     the first day, so outline fonts (2.8a′) change no widget.
     **Design goals** (Aurora, 2026-11-11), where other systems are weak:
     (1) one toolkit, no legacy layers beside it; (2) logical units and a
     scale per window from the first day, snapped to device pixels, so
     fractional and mixed scales stay sharp; (3) a UI that does not freeze:
     animation runs in the compositor's springs, and file and network calls
     come back as events, never blocking the UI thread; (4) accessibility in
     the widget tree (role, name, state on every widget; keyboard navigation
     everywhere; high contrast, reduced motion, the Atkinson switch; the
     whole plan: `system/CX_ACCESSIBILITY.md`);
     (5) unforgeable windows: the server-drawn frame names the verified
     signer, and an indicator shows a held capture grant; (6) the file
     picker as a capability: a system dialog that hands back a handle to the
     chosen file only; (7) the clipboard read only on a user's paste or
     consent; (8) every app's commands registered with the toolkit, one
     system-wide command palette, consistent shortcuts; (9) a standard
     inspector (tree, bounds, damage) and an automation API over the
     accessibility tree, so UI tests run in verification boots on the
     virtual screen and are sabotaged like the rest. And a design system the
     toolkit enforces: a 4/8 px spacing scale, one radius, one shadow, one
     accent, light and dark, grayscale antialiasing, kerning and tabular
     figures. 1-4 and the design system are 2.8a (costly to retrofit); 8-9
     come with the terminal and the Process Manager; 5-7 follow the
     programs that need them (6 needs Phase 3's files).
     **Localisation and text input** (2026-11-11), surveyed with the rest
     (Haiku's input_server and locale kit, Serenity's LibGUI, Windows' TSF):
     every user-visible string is a message looked up by key, never by its
     English text, with placeholders and plural choices after Mozilla's
     Fluent design (Apache-2.0 spec). Translators write a text source per
     language; `cxk` compiles it to a string table of fixed records, read
     through a checker; lookup falls back to English. Plural rules, date,
     number and currency formats come from Unicode's CLDR (data, vendored and
     converted). Right-to-left text: our own Unicode bidirectional algorithm
     (UAX #9), run against Unicode's conformance files, with mirrored
     layouts. A pseudo-locale (accented, 40% longer, mirrored) runs in the
     verification boots, so a hard-coded string, a clipped label or a broken
     mirror fails, and is sabotaged. The text field takes input through an
     input-method interface from the first day (keyboard layouts, dead keys,
     composition), so CJK input needs no retrofit.
     **Keybinds** (Aurora, 2026-11-11). Ctrl is for app commands, Super for
     the system. Every shortcut is a registered command (goal 8): menus and
     the palette show it, a clash is refused when an app registers, and the
     user rebinds anything in one place, except three fixed keys:
     Ctrl+Alt+Del (the trusted screen), Super+Ctrl+Enter (the screen reader,
     so no setting can lock a user out) and Ctrl+Alt+F2 (the console
     fallback). Apps can never capture the system's keys (Super, Alt+Tab,
     Alt+F4, the fixed three) whatever the user binds them to; a fullscreen
     game may capture Super with consent, never Alt+Tab or the fixed three.
     Defaults, system: Super (tap) the launcher; Super+Enter a terminal;
     Alt+Tab / Alt+Shift+Tab switch windows; **Alt+F4 closes the window**:
     a close request the app answers (it may ask to save), and a window that
     does not answer within a few seconds gets the system's "not
     responding: wait or end it" dialog; on the desktop it opens the
     shut-down dialog. Super+arrows snap and maximise; Super+D the desktop;
     Super+L lock; Super+Shift+S and PrtSc a screenshot through the
     compositor (trusted screens never in it); Ctrl+Shift+Esc the Process
     Manager; Super+, settings; Super+Plus/Minus zoom. Apps: Ctrl+C X V A,
     Ctrl+Z and Ctrl+Shift+Z, Ctrl+S O N W Q F T, Ctrl+Tab, F2, F5,
     Ctrl+Shift+P the app's palette. The terminal copies with Ctrl+C only
     when text is selected, else interrupts; Ctrl+Shift+C/V always copy and
     paste. Letters follow the layout; a layout without Latin letters falls
     back to the US positions for shortcuts. A verification-boot check
     fails if two commands in the shipped apps share a key, and is
     sabotaged. A broken binding set is reset from the trusted screen.
     `os/std/gui.xfxn`, the older toolkit that draws straight to the
     framebuffer (`SYS_FB_*`, `SYS_MOUSE_READ`) and so bypasses displayd:
     **retired** (keep any drawing worth having; `cxos.xfxn` moved onto
     windows or dropped; listed in `TRANSITIONAL_CODE.md` meanwhile).
   - **2.8a′, scalable fonts** (`system/CX_FONTS.md` §3 item 5; defaults
     IBM Plex Sans for the UI, Space Grotesk for headings, IBM Plex Mono for
     the terminal, Atkinson Hyperlegible Next for accessibility, with many
     more shipped as choices; all OFL-1.1). **Our own outline format, so the OS never
     parses TrueType** (Aurora, 2026-11-11): `cxk font build` reads the TTF
     on the host, where a parser bug costs a build and not a machine, and
     writes XFNT v2, a kind field beside v1's bitmap: per glyph its advance,
     its bounds and its contours as quadratic segments in fixed point, plus
     the codepoint table. The host does everything hard: `cmap` formats,
     composite glyphs flattened, CFF's cubics turned into quadratics (so
     either Noto build will do), hinting dropped. The OS side reads fixed
     records and checks every offset, count and coordinate against the file
     and the em box, the CXFS superblock check's standard, with sabotages
     feeding it bad files; fonts load only from `/System`. The rasteriser
     (X, over `fixed.xfxn`): flatten, fill with antialiased coverage,
     cache glyphs per size. A C# rasteriser of the same algorithm in the
     DevKit is the oracle, as the C# compiler is for `os/xc`. A converted
     font is a Modified Version: it gets our own face name where its
     licence reserves one (Plex, probably; check each when it is vendored). A UI scale setting comes
     with it. The terminal defaults to Plex Mono. A choice of faces,
     proportional and monospace, with a core in the image and the rest as
     packages later: `CX_FONTS.md` §3 item 5 (ligatures a later step).
     Icons with it (`system/CX_ICONS.md`): Phosphor's symbols as XFNT v2
     glyphs by name, on the same rasteriser; colour and file-type icons
     with the first system apps.
     Open: `CX_FONTS.md` puts the
     rasteriser in the display server, but windows are client-drawn
     surfaces, so the toolkit is where text is drawn; decide in 2.8a and
     update `CX_FONTS.md`.
   - **2.8b, the terminal** (`os/apps`): a window with the text grid; the
     shell runs under it with its standard input and output as pipes. The
     shell today takes the keyboard (`GRANT_INPUT`) when it has no stdin
     handle; under a terminal it reads the pipe, and the terminal turns
     window key events into bytes. Needed with it: line editing over a
     pipe, scrollback, resize, and a way to interrupt a program (no Ctrl-C
     or `kill` in the shell yet).
   - **2.8c, the default session:** the executive starts displayd and a
     terminal instead of the console shell, and falls back to the console
     shell when displayd cannot start or ends. Esc / Ctrl+Alt+Del behaviour
     is kept. `cxk verify-boot --full` looks for the shell's `cxk> ` on
     serial; under a terminal it goes to a pipe, so the boot verdict needs
     another witness (a serial mirror, or a kernel-visible "session up").
     **The desktop** (Aurora, 2026-11-11): a launcher opened by Super that
     searches first (apps, files, settings and every app's registered
     commands, so it is also the system command palette), with pinned apps
     and recent files beneath; never ads or web results. A taskbar of
     running windows. The trusted strip (clock, camera, microphone and
     capture indicators) is drawn by the compositor, so no app can fake it.
     **Always on top** (Aurora, 2026-11-11): any window, set by the user
     from the frame's menu or Super+Ctrl+T (PowerToys' key), shown by a pin
     on the frame, remembered per app as a setting. The compositor owns the
     stacking, in three layers: normal, the user's always-on-top windows,
     and the system's own (the trusted strip, consent screens, the
     Ctrl+Alt+Del screen) above both. System apps (platform-signed) carry
     it as a built-in option, as Task Manager's "Always on top" (the
     Process Manager first). Any other app is pinned only by the user, from
     the frame or the key; it can neither set nor offer it (on Windows
     `HWND_TOPMOST` is a pop-up's tool). Picture-in-picture video is the
     toolkit's form of it.
     Survey first: Haiku's Deskbar, Serenity's Taskbar and Assistant,
     Windows' Start, macOS's Spotlight and Dock. The terminal comes first;
     the launcher and taskbar follow it in this step or the next.
   - **2.8d, the rig:** 2.5 to 2.8 on the FX-8350 (none of it has run there).
   - **2.8e, the compositor API, public and stable** (Aurora, 2026-11-11),
     after the toolkit, its first user. What 2.4-2.5 built, for anyone: windows,
     retained layers and their properties, springs run by the compositor
     (state the target, the compositor animates), a frame clock (the next
     frame's time, and when each commit reached the screen), layers whose
     pixels come from a lease (video, without a copy), and named, versioned
     effects (blur, shadow, rounded clip), not arbitrary shaders. No private
     path: the system's own UI uses the same calls. **One protocol, one
     definition:** an IDL file beside `abi/kernel.xfil` from which `cxk`
     generates the client library and checks compatibility as `check-abi`
     does; versioned whole, never a set of optional extensions (Wayland's
     fragmentation is the thing avoided). With it the SDK: generated
     bindings, documents, samples, a stability policy (versions, how long a
     deprecated call lives, how an app says what it needs) and conformance
     tests in the verification boots.

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

**Media** (Aurora, 2026-11-11), with audio; images earlier, when the toolkit
wants icons. **Royalty-free formats only, every reader our own:** no vendored
codec library, and no format that needs a patent licence (H.264, HEVC and AAC
are out). Images: PNG (with our own inflate), JPEG, QOI. Audio: PCM/WAV, FLAC,
Opus, MP3 (its patents have expired). Video: AV1, VP9. Containers: Ogg,
WebM/Matroska, ISO BMFF (MP4) for the royalty-free codecs in it. **Decoding
never runs in the app:** every decoder and container reader is its own process
with no grants, handing raw frames back through leases, and its output is
checked before it is shown (Android's Stagefright and iOS's BlastDoor are why;
they isolated decoding after the fact). Tested against the formats' published
conformance files (PngSuite, the AV1 and Opus test vectors and the like, under
`vendor/` with their licences), and sabotaged like the rest. A small pipeline,
not a general graph: source, decoder, sink, one clock for audio and video;
apps ask for "play", "decode a frame" and "capture with consent" (the 2.6
consent screen, an indicator while it lasts). Video in X on the CPU will be
slow until there is hardware decode: images and audio first, video playback a
later milestone.

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
- ~~Whether `setcheck.xuex` should write on every boot.~~ Decided 2026-11-08: only when asked (§1).
