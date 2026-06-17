# The CXK Process Model — Ring 3, Scheduling & Preemption
### CXK Reference — Aurora Tejeda / CATX SYSTEMS LLC

This document describes CXK's process model: how the kernel drops to user mode
(ring 3), the syscall interface, the scheduler, and how preemptive multitasking
of user processes works. It records not just the design but the non-obvious
decisions and pitfalls, since several of them are subtle enough to be costly to
rediscover.

> **Status:** Implemented and validated on hardware/emulator. What exists today:
> ring 0/3 privilege separation, a syscall gate, cooperative and preemptive
> context switching, process lifecycle (create/exit/reap), and multiple
> preemptible ring-3 processes sharing one address space. **Not yet done:**
> per-process address spaces (separate page directories / CR3 switching) — that
> is a future checkpoint (3b). All processes currently share the kernel's single
> address space, isolated only by per-process user-page regions.

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

Implemented syscalls (`cpu/usermode.c`, `syscall_dispatch`):

| # | Name        | Args                | Behavior                                   |
|---|-------------|---------------------|--------------------------------------------|
| 0 | `SYS_EXIT`  | `ebx` = exit code   | returns control to the kernel              |
| 1 | `SYS_WRITE` | `ebx` = ptr, `ecx`=len | writes a string (len 0 = bounded NUL-scan) |
| 2 | `SYS_GETPID`| —                   | returns current pid                        |

**Pointer validation:** because there is now a privilege boundary, the kernel
must never blindly dereference a pointer handed up from ring 3. `user_ptr_ok`
validates that a user buffer lies within the *calling process's* user region
(see §6), bounds the length, and rejects overflow.

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

## 6. Per-Process User Memory

Each ring-3 process is assigned a 64 KB user-page slot (base `0x800000`, indexed
by pid). The trampoline maps a user code page and a user stack page with
`PAGE_USER` set. Two paging subtleties apply:

- The user/supervisor bit is enforced at **both** paging levels. `paging_map`
  must OR `PAGE_USER` into the **page-directory entry** as well as the page-table
  entry, or a ring-3 access faults even though the PTE allows it.
- Processes share the kernel's single page directory; isolation is by *region*
  (each process only maps its own slot user-accessible). True per-process
  address spaces are a future checkpoint.

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

The `ringtest` shell command exercises the model (each subtest is also a
regression test):

| Subtest            | Exercises                                       |
|--------------------|-------------------------------------------------|
| `ringtest user`    | ring-3 entry, syscall, clean return             |
| `ringtest threads` | cooperative kernel-thread context switching     |
| `ringtest preempt` | timer-driven preemption of kernel threads       |
| `ringtest proc [N]`| N cooperative ring-3 processes, lifecycle/reap  |
| `ringtest procp`   | two preemptive ring-3 processes (full model)    |
| `ringtest all`     | runs the cooperative suite in sequence          |

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

- **3b — Per-process address spaces:** a page directory per process, CR3
  switching on context switch, the kernel mapped into every address space. This
  brings true memory isolation (a process cannot read another's memory at all,
  not merely "is not mapped it"). It is the largest remaining piece and is
  intentionally deferred.
- **Program loading:** running user code loaded from disk (CXFS) rather than
  copied from a built-in blob.
- **System-wide preemption:** making the shell itself a scheduled thread, which
  additionally requires console locking and preemption-safe drivers.