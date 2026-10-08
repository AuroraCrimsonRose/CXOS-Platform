# The CXK Process Model — Ring 3, Scheduling & Preemption
### CXK Reference — Aurora Tejeda / CATX Systems

This document describes CXK's process model: how the kernel drops to user mode
(ring 3), the syscall interface, the scheduler, and how preemptive multitasking
of user processes works. It records not just the design but the non-obvious
decisions and pitfalls, since several of them are subtle enough to be costly to
rediscover.

> **Status:** Implemented and validated on hardware/emulator. What exists today:
> ring 0/3 privilege separation, a syscall gate, cooperative and preemptive
> context switching, process lifecycle (create/exit/reap), multiple preemptible
> ring-3 processes, and — **as of checkpoint 3b, now complete** — true
> per-process address spaces: a page directory per process, CR3 switched on
> every context switch, the kernel half mapped into every space. A process
> cannot address another's memory at all.
>
> **What remains** (see §12): preemption is implemented but **not enabled in the
> boot path** (`sched_preempt_enable()` is called only from `ktest.c`), because
> system-wide preemption additionally needs console/framebuffer arbitration and
> preemption-safe drivers. And programs are still compiled into the kernel image
> rather than loaded from disk.

---

## 1. Overview

A **process** in CXK is a scheduler thread whose kernel-stack trampoline drops
it into ring 3. This unifies two mechanisms:

- **The scheduler** switches *kernel* stacks via `context_switch` (a function-
  call-based switch).
- **Ring 3 entry** uses an `iret`-based transition (`enter_usermode`).

Rather than treat these as separate worlds, every process is a normal scheduled
thread; what makes it a *user* process is that its entry function is a kernel
**trampoline** (`process_trampoline`) which sets up user pages and calls
`enter_usermode` to drop to ring 3. When the user code exits (via a syscall),
the trampoline returns and calls `thread_exit()`, and the scheduler reaps it
like any other thread.

This means the proven context-switch path is reused unchanged for user
processes — the privilege transition is layered *on top of* the scheduler, not
bolted into it.

---

## 2. Privilege Separation (GDT + TSS)

The kernel installs its own GDT (`cpu/gdt.c`), replacing the bootloader's, with
six descriptors:

| Selector | Index | Purpose                  | DPL |
|----------|-------|--------------------------|-----|
| `0x00`   | 0     | null                     | —   |
| `0x08`   | 1     | ring-0 code              | 0   |
| `0x10`   | 2     | ring-0 data              | 0   |
| `0x18`   | 3     | ring-3 code (sel `0x1B`) | 3   |
| `0x20`   | 4     | ring-3 data (sel `0x23`) | 3   |
| `0x28`   | 5     | TSS                      | 0   |

The ring-0 selectors are kept identical to the bootloader's, so existing kernel
code is unaffected by the GDT reload.

The **TSS** matters because when an interrupt or syscall occurs while the CPU is
in ring 3, the CPU switches to the ring-0 stack named by `TSS.esp0`/`TSS.ss0`.
Without a valid TSS, the first interrupt taken in user mode triple-faults.

---

## 3. Syscall Interface

User code cannot call kernel functions or touch hardware directly — it would
fault. It requests services through a software-interrupt gate:

- **Vector `int 0x80`**, installed with **DPL=3** (`idt_set_user_gate`) so ring
  3 is permitted to invoke it.
- Convention: syscall number in `eax`, args in `ebx`/`ecx`, return value in
  `eax`.

There are **16 implemented syscalls** (`cpu/usermode.c`, `syscall_dispatch`), spanning
lifecycle, IPC, handles, input, console, framebuffer, network, spawn and power.

**They are not listed here.** `abi/cxk_abi.h` is the source of truth and `docs/kernel/CX_ABI.md`
is the reference; duplicating the table in this document is how it went stale — it sat at
three entries long after the real count reached sixteen, and misdescribed `SYS_WRITE` as
number 1 when `SYS_CONSOLE_WRITE` is `0x30`. See **`docs/kernel/CX_ABI.md` §7**.

The part that belongs *here* is the boundary rule, not the call list:

**Pointer validation:** because there is now a privilege boundary, the kernel
must never blindly dereference a pointer handed up from ring 3.
`user_ptr_readable` and `user_ptr_writable` validate that a user buffer lies
within the *calling process's* user region (see §6), bound the length, and
reject overflow; which one to call depends on the access intended, and there is
no combined form.

Validation alone is not enough for anything that blocks in between, because it
is only true at the instant it runs. Copies across the boundary go through
`user_copy_out` / `user_copy_in`, which validate at the copy and survive a
fault inside it. `CX_ABI.md` §3 has the rule and the reasoning.

---

## 4. Entering and Leaving Ring 3

`enter_usermode(entry_eip, user_esp, save_slot)` (`cpu/usermode.asm`):

1. Saves the kernel's callee-saved registers and EFLAGS, and stores the kernel
   `esp`/`eflags` into the per-process **save slot** (see below).
2. Loads the ring-3 data selectors.
3. Builds an `iret` frame (user `SS`, `ESP`, `EFLAGS` with IF set, user `CS`,
   entry `EIP`) and executes `iretd` → ring 3.

`SYS_EXIT` routes through `return_to_kernel(retval, save_slot)`, which restores
the kernel `esp` from the save slot, pops the callee-saved registers, restores
EFLAGS, and returns — making `enter_usermode` appear to return the exit code.

**Per-process save slot.** `enter_usermode`/`return_to_kernel` do *not* use a
global to hold the saved kernel esp/flags — they use a 2-word slot inside the
process struct (`u_saved_esp`, `u_saved_flags`), passed as `save_slot`. This is
required for preemption: if process A is preempted while in ring 3 and process B
then enters ring 3, a global would be overwritten and A's eventual `SYS_EXIT`
would restore B's state. Per-process storage keeps each process's return path
intact.

---

## 5. Scheduler

`cpu/sched.c` maintains a fixed table of threads (`MAX_THREADS`). Each has its
own kmalloc'd kernel stack, a saved `esp`, a state
(`UNUSED`/`READY`/`RUNNING`/`EXITED`), and process fields (pid, exit code,
user/kernel flag).

- **`context_switch(old_esp, new_esp)`** (`cpu/switch.asm`) saves the current
  thread's callee-saved registers + flags, swaps `esp`, and restores the next
  thread's. The initial stack of a new thread is hand-crafted so the first
  switch into it lands at a launch trampoline that calls the entry function.
  The hand-built layout **must mirror the restore order exactly**.
- **`yield()`** — cooperative: round-robins to the next ready thread.
- **`thread_exit()`** — marks the thread `EXITED` and switches away.
- **Reaper (`sched_reap`)** — a thread cannot free its own stack (it is running
  on it), so `thread_exit` only marks the slot; the reaper, called *after* a
  switch (from a different thread's context), frees the dead thread's kernel
  stack, user stack, and esp0 stack, then marks the slot reusable. This is what
  makes process creation/teardown leak-free.

---

## 6. Per-Process Address Spaces (checkpoint 3b — complete)

Each ring-3 process gets its **own page directory**. `proc_start` calls
`addr_space_create`, records the directory's physical address on the thread
(`thread_set_space(pid, space.pd_phys)`), and the scheduler loads it into CR3 on
every switch (`update_address_space`). Isolation is now structural: a process
cannot *name* another process's memory, let alone read it.

The kernel half (`0xC0000000`+) is mapped into every space, so kernel code and
data are reachable no matter which directory is live. `addr_space_init_pd` zeroes
the whole user half for a new process, and a PDE hook propagates newly created
shared-kernel-half PDEs into already-live spaces so a late kernel mapping is
visible everywhere.

Three paging subtleties apply, each of which has cost real debugging time:

- The user/supervisor **and write** bits are ANDed across **both** paging levels.
  `paging_map` must OR `PAGE_USER` into the **page-directory entry** as well as
  the page-table entry, or a ring-3 access faults even though the PTE allows it.
- **Anything the kernel must reach from inside a process's address space has to
  live in the kernel half.** An identity mapping made at boot exists only in the
  kernel's own directory, so it vanishes the moment a syscall runs with a
  process's CR3 loaded. This is exactly how the e1000 DMA region faulted
  (`CR2 = 0x712000`) once `ping` was issued from the shell rather than from boot
  context: the fix was to alias the region into the kernel half and keep handing
  the NIC the physical address.
- A device's MMIO window must likewise be kernel-half or it is invisible from a
  process. PCI BARs normally land high, which is why identity-mapping them
  happens to work; `e1000_init` now refuses a BAR below `0xC0000000` rather than
  faulting mysteriously later.

Teardown is symmetrical and leak-free: on exit the trampoline reclaims the user
frames and page tables *while still in that space*, switches to the kernel space,
then destroys the directory (`addr_space_reclaim_user` → `addr_space_switch` →
`addr_space_destroy`).

---

## 7. Preemption

Preemption lets the timer interrupt switch threads without them yielding.

`timer_callback` → `sched_tick()` runs on every timer IRQ. When enabled, every
`quantum` ticks it sets a `need_resched` flag — it does **not** switch directly.
The actual switch happens in `sched_preempt_point()`, which the IRQ handler
calls **after** sending the PIC end-of-interrupt.

> **Critical: EOI ordering.** The context switch must happen *after*
> `pic_send_eoi`. If the switch happens first, control leaves the IRQ handler
> before the EOI is sent, the PIC's in-service bit for the timer stays set, and
> no further timer interrupts are delivered — preemption stalls after exactly
> one switch. Deferring the switch to `sched_preempt_point` (post-EOI) keeps
> ticks flowing.

Because the timer IRQ stub already saves the caller-saved registers (`pushad`)
and `context_switch` preserves the callee-saved set, the full state of a thread
preempted at an arbitrary instruction is intact and it resumes exactly where it
was interrupted.

---

## 8. Preemptible Ring-3 Processes — The Hard Part

Combining ring 3 with preemption requires per-process kernel stacks for the
`esp0` path:

- **Per-process esp0 stack.** Each ring-3 process gets a *second* kernel stack
  (`thread_alloc_kstack`), distinct from its trampoline stack. When the timer
  interrupts a process in ring 3, the CPU switches to this stack to push the
  interrupt frame — so it never collides with the trampoline's saved frame on
  the process's main kernel stack.
- **`update_tss_esp0`** is called on every context switch to point `TSS.esp0`
  at the *currently scheduled* process's esp0 stack. Without this, a syscall or
  interrupt taken by process B while A is still mid-ring-3 would land on the
  wrong stack and corrupt state.
- Kernel-only threads leave `kstack_top` at 0; for them `update_tss_esp0` is a
  no-op and the single dedicated TSS stack set in `gdt_init` is used (this also
  serves the non-scheduled `usermode_test` path).

Trace of a preemption during ring 3:

1. Timer fires while process A is in ring 3. CPU switches to A's esp0 stack and
   pushes A's ring-3 interrupt frame.
2. IRQ stub saves registers; `timer_callback` flags `need_resched`.
3. IRQ handler sends EOI, then calls `sched_preempt_point`, which
   `context_switch`es to B (saving A's esp0-stack esp into `A.esp`).
4. Later, the scheduler switches back to A: its esp is restored, the IRQ stub
   tail runs, and `iret` returns A to exactly where it was in ring 3.

---

## 9. Fault Handling

CPU exceptions are checked for privilege: if a fault's saved `CS` has RPL 3
(`(cs & 3) == 3`), it occurred in ring 3. A registered hook
(`usermode_fault`) then terminates the offending process (via `thread_exit`,
which reschedules) instead of panicking the kernel. Faults from ring 0 still
panic, as they indicate a kernel bug. The hook is registered with
`set_user_fault_hook` and kept out of `idt.c`'s direct dependencies so the
interrupt layer does not hard-depend on the process model.

---

## 10. Testing

The old `ringtest` shell command is gone — the shell was rewritten in X and did
not carry it over. The tests moved somewhere better: **`kernel/ktest.c`, run
automatically from `kmain` on every boot** (`ktest_run()`), before the executive
is launched. A regression cannot be forgotten because nobody typed the command.

| Self-test | Exercises |
|-----------|-----------|
| `test_paging` | map/unmap, recursive directory |
| `test_heap` | kmalloc/kfree |
| `test_sched_coop` | cooperative kernel-thread context switching |
| `test_sched_preempt` | timer-driven preemption of kernel threads |
| `test_ring3_single` | ring-3 entry, syscall, clean return (3a) |
| `test_ring3_processes` | cooperative *and* preemptible ring-3 processes (3b/3c) |
| `test_identity` | a user process can never be UID 0 |
| `test_storage` | disk read/write |
| `test_cxfs` | filesystem format/mount/create/read |
| `test_pci` | bus enumeration |
| `test_ahci` | AHCI bring-up |

`test_sched_preempt` and `test_ring3_processes` each call `sched_preempt_enable(5)`
and then `sched_preempt_disable()`. So **preemption is proven on every boot and
then deliberately switched back off** — the model is validated, and normal
operation is cooperative by choice, not because preemption is unfinished. §12
records what turning it on permanently still needs.

---

## 11. Recurring Lessons

- The user/supervisor (and write) bits are ANDed across **both** paging levels —
  set them on the PDE and the PTE.
- `TSS.esp0` must be a stack **separate** from the kernel's working stack, and
  for concurrent ring-3 processes, separate **per process**.
- Preserve and restore EFLAGS across privilege transitions, or the kernel can
  return with interrupts disabled and deadlock on its next `hlt`.
- A preemptive switch from an IRQ must happen **after** the EOI.
- The hand-crafted initial thread stack must mirror `context_switch`'s restore
  order exactly.
- Validate every pointer handed up from ring 3 before dereferencing it.

---

## 12. Roadmap

- ~~**3b — Per-process address spaces.**~~ **Done.** See §6. A page directory per
  process, CR3 switched on every context switch, the kernel half mapped into every
  space, leak-free teardown. This was the largest remaining piece.

Remaining, in the order they unblock each other:

1. **Program loading from disk.** Running user code read from CXFS rather than
   copied from a blob compiled into the kernel image. Today the entire userland
   is one embedded array (`os/executive/app_image.h`) holding a single image, and
   `spawn` takes an in-memory image rather than a path — so the shell and the GUI
   are statically linked into *one* binary and cannot launch separate application
   files. The kernel side already exists: `cxk_launch_executive(path)` reads a
   CXEX from CXFS and hands it to `cxex_exec`, which verifies its signature. What
   is missing is the syscall exposing it (`docs/kernel/CX_ABI.md` §7.10) and filesystem
   access from ring 3. **This is the top priority**: it is what makes an
   application ecosystem possible at all, and because it routes through
   `cxex_exec`, loading apps from disk inherits signature verification for free.

2. **Display and console arbitration.** Preemption is already proven (§10); what
   it lacks is drivers that tolerate it. With two ring-3 processes runnable, both
   can be mid-`console_write` or mid-`fb_op`, and the framebuffer has no notion of
   ownership or clipping per process. Needs either a lock per device or — better,
   and the direction the GUI is already heading — a display server holding
   `GRANT_FRAMEBUFFER` that clients draw through by IPC.

3. **System-wide preemption:** enable it in the boot path once (2) holds, making
   the shell an ordinary scheduled thread alongside everything else.

4. **CP4 hardening** (`docs/kernel/CX_ABI.md` §11): IPC lifecycle edge cases — a caller
   dying mid-call, an executive dying with a caller blocked — handle cleanup on
   exit, and message-bound fuzzing.