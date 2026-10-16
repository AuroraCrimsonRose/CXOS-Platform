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
| **The 32-bit kernel keeps building in CI until 64-bit catches up, then retires as a target.** | It costs little while CI runs it, and it keeps the code honest about what is architecture-specific. |
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

1. **Kernel object model.** One header for every kernel object: type,
   reference count, rights, signalled state. Each handle records the grant it
   came from (§5). Every syscall and IPC message passes one recording point.
   See `kernel/CX_KERNEL_PLAN.md` §2.
2. **Architecture split.** `kernel/arch/x86_32` and `kernel/arch/x86_64`
   beneath generic code; paging behind one interface (Haiku's model).
3. **Processes separate from threads.** Handle table, capabilities, uid, cwd
   and address space move from the thread to a process object. Thread and
   handle tables become dynamic.
4. **X for x86-64.** `usize` / `isize`, a target layer in both compilers, a
   64-bit code generator held to the C# oracle by the differential tests, and
   syscalls through the `syscall` instruction. See `language/CX_X_PLAN.md`.
5. **Boot hands over through CXBI.** The kernel reads `cxk_boot_info` instead
   of fixed addresses, and the BIOS loader enters long mode and starts a
   64-bit kernel. See `boot/CX_BOOT.md`.
6. **The x86-64 kernel boots.** Long-mode paging with NX, the 64-bit GDT, IDT
   and TSS (with IST for the double fault), `syscall`/`sysret`, and the self-test
   suite passing on x86-64.
7. **Stage 5: assembler and linker in X, multi-target from the start.**

Independent of the order above, and small enough to land whenever convenient:

- **Close the input hole:** `SYS_INPUT_READ` and `SYS_MOUSE_READ` need no
  capability today, so any process can read keystrokes meant for another.
- Stack protector on, seeded from boot entropy.
- A crash log that survives a reboot.
- Find the intermittent first-boot install failure (about 1 in 20 boots,
  `CXK_INSTALL_FORMAT_ERR`), using QEMU's whole-machine record and replay.

### Phase 1: processes, waiting and IPC

`kernel/CX_KERNEL_PLAN.md` §3–§7.

1. Process handles: wait, stop, kill, inspect; detailed exit reasons; jobs with
   memory and thread budgets.
2. The wait queue, and timers as handles (LAPIC timer and TSC).
3. IPC: queued calls with reply tokens, badges, handle transfer, sealable
   memory objects, notifications, priority passed through calls.
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

Carried from x86-32, now on 64-bit: X Data (done), FAT32 on the VFS, X Shell
and a terminal, the Process Manager, basic apps, networking (UDP, then DHCP
and DNS, then TCP). Audio (`system/CX_AUDIO.md`) joins here.

### Later

Vulkan through Venus on virtio-gpu; the native AMD driver; the IOMMU (QEMU
first; the rig's chipset has none); the ecosystem phase (package repository,
key distribution, in-place updates); then ARM.

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
- The ink polarity question in `devkit/CX_DEVKIT_DESIGN.md` §5.1.
- Whether `setcheck.xuex` should write on every boot.
