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
| No process object | Handle table, caps, uid, cwd and address space are on the **thread** (`sched.h`); a process is one thread | §3 |
| Fixed small tables | 64 threads system-wide, 8 by default; 16 handles per process | §3 |
| No way to learn a child's exit, or to kill | `sched.c` stores an exit code nothing returns | §4 |
| One thing waited on at a time | input, one endpoint, or sleep | §5 |
| Narrow IPC | one call in flight per endpoint, one page copied, no handle passing, no shared memory | §6 |
| No user threads, no futex, no pipes | | §7 |
| x86 code mixed with generic code | `kernel/cpu` holds both `gdt.c` and `sched.c` | §2.2 |
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

### 2.2 Architecture split

`kernel/arch/x86_32/` and `kernel/arch/x86_64/` hold the CPU-specific code
(GDT, IDT, TSS, context switch, entry stubs, paging); everything else is
generic. Paging sits behind one interface with one implementation per
architecture, as Haiku does (`src/system/kernel/arch/x86/paging/{32bit,pae,64bit}`).
XNU's `osfmk/i386` beside `osfmk/kern` and NT's HAL are the same idea.

### 2.3 x86-64 bring-up

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
| Allocator | Size-class slab allocator, per-CPU caches later; debug builds poison freed memory, add guard zones and catch double frees |
| Time | LAPIC timer and TSC; program the next deadline instead of a fixed tick; HPET as fallback |
| Hardening | Stack protector; KASLR seeded from `cxk_boot_info.rng_seed`; SMEP and SMAP where CPUID reports them; kernel UBSan in debug builds (Serenity `Kernel/Prekernel/UBSanitizer.cpp`); control-flow integrity later |
| Diagnostics | A crash log in reserved RAM read on the next boot; symbolised backtraces; a GDB stub over serial; a timestamped trace buffer; per-thread CPU time |
| Scheduler | Priority classes (real-time for input and audio, normal, background); per-CPU run queues; HLT when idle |
