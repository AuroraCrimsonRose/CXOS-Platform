# CXK Engineering Review

**Review scope:** non-security engineering and architecture observations from the `x86_32_DEV` kernel/runtime review  
**Focus:** correctness, subsystem contracts, maintainability, scheduler architecture, storage, compiler tooling, lifecycle/ownership, ABI design, and test coverage  
**Excluded:** security findings covered by `docs/SECURITY_REVIEW.md`  
**Review date:** 2026-10-01

> **Response & status:** every finding below has been checked against the code
> and planned in [`docs/planning/HARDENING_PLAN.md`](../../planning/HARDENING_PLAN.md), which tracks each to
> done. This review is kept as written.

## Executive summary

The current CXK codebase is moving beyond the "kernel experiment" stage into a system where subsystem contracts matter as much as individual implementations. Most of the observations below are not urgent defects; they are places where an explicit contract now will prevent future complexity as CXK gains more concurrency, storage, filesystem, compiler, and user-space functionality.

The main engineering priorities are:

1. Tighten the generic block-device/disk API so backend limitations cannot silently truncate requests.
2. Formalize the scheduler's cooperative/preemptive lifecycle and its transition toward concurrent ring-3 execution.
3. Complete or clearly gate per-process `esp0` handling before multiple ring-3 contexts can execute concurrently.
4. Harden lexer arithmetic and preserve a clearly defined source-of-truth relationship between the X and reference lexers.
5. Establish explicit buffer/parameter contracts for low-level string and formatting APIs.
6. Expand timing tests to verify both minimum and reasonable maximum sleep latency.
7. Stabilize the block-device contract before deeper VFS/filesystem/page-cache work accumulates assumptions around it.
8. Make ownership, initialization order, ABI boundaries, and failure semantics explicit before the subsystem count grows further.

---

## 1. Disk API: generic interface is wider than the ATA backend

### Observation

The generic disk layer accepts a 64-bit LBA and 32-bit sector count, but the ATA backend is invoked with a 32-bit LBA and an 8-bit count.

This creates a mismatch between what the abstraction promises and what one backend can actually represent. A request larger than the ATA backend's native range can be silently truncated rather than rejected or split.

### Recommendation

Make backend capabilities explicit. For ATA, either:

- reject LBAs/counts outside the backend's supported range with `DISK_ERR_PARAMS`, or
- split large operations into backend-sized requests where the hardware permits it.

The generic API should document whether `count` represents sectors, bytes, or an implementation-defined maximum, and each backend should expose its maximum transfer size where useful.

A long-term block-device interface could expose:

```c
struct block_ops {
    uint64_t max_lba;
    uint32_t max_sectors;
    uint32_t sector_size;
    int (*read)(...);
    int (*write)(...);
};
```

This avoids making callers learn backend-specific constraints indirectly.

---

## 2. Disk/string formatting API should have explicit buffer contracts

### Observation

Low-level formatting routines such as disk capacity formatting accept a caller-provided buffer and capacity, but edge-case behavior for zero or extremely small capacities should be explicit.

### Recommendation

Adopt one kernel-wide convention for bounded string APIs. For example:

- `cap == 0` means no write and no dereference;
- successful output is always NUL-terminated when `cap > 0`;
- truncation has a defined return value;
- `NULL` is either explicitly permitted or explicitly rejected.

Apply the same convention across string/formatting helpers rather than having each subsystem invent its own behavior.

---

## 3. Scheduler architecture is entering a transition point

### Observation

CXK currently contains cooperative scheduling through `yield()` plus an optional timer-driven preemption mechanism. The process model documentation describes enabling preemption for validation and then deliberately disabling it again for the normal current model.

This is reasonable during development, but the distinction between cooperative scheduling, timer preemption, ring-3 execution, timer wakeups, and future SMP scheduling is becoming important enough to formalize.

### Recommendation

Document the scheduler lifecycle explicitly, for example:

```text
BOOT
  -> cooperative kernel threads
  -> validated preemption
  -> preemptive single-CPU user execution
  -> concurrent user threads
  -> SMP
```

Define which invariants change at each stage and which scheduler APIs are legal in each mode.

A single explicit scheduler configuration/state model would be preferable to making callers infer the active mode from several independent flags.

---

## 4. Per-process kernel stacks need to be a defined milestone

### Observation

The scheduler already has infrastructure to update the TSS `esp0`, but newly-created threads can have `kstack_top == 0`; comments describe the current single-dedicated-TSS-stack arrangement as valid while only one process is executing in ring 3.

### Recommendation

Keep this limitation explicit in `PROCESS_MODEL.md` and treat per-process `esp0` as a prerequisite for concurrent ring-3 execution.

Before enabling multiple simultaneous user contexts, define the invariant:

```text
current ring-3 thread -> its own kernel entry stack -> its own saved kernel context
```

This should become a scheduler/process contract rather than remaining an implementation detail of the current transition phase.

---

## 5. X lexer arithmetic should use overflow-safe indexing

### Observation

The X lexer uses expressions of the general form `position + lookahead >= length` when checking lookahead bounds.

For normal source this is harmless, but the lexer is a parser-facing component that is explicitly tested with mutated/adversarial input. Overflow-safe range checks are inexpensive and make the primitive robust by construction.

### Recommendation

Prefer subtraction-based checks:

```c
if (pos >= len) ...
if (lookahead > len - pos) ...
```

Apply the same pattern to every lexer/parser buffer range calculation.

---

## 6. Define the authoritative lexer during the X transition

### Observation

The X lexer is maintained against a C# reference lexer using differential tests. This is useful for compatibility and regression detection, but it creates a future question: which implementation defines the language once X begins evolving beyond the reference language?

### Recommendation

Document the relationship explicitly:

- while X is compatible with the reference language, the reference lexer is the behavioral oracle;
- intentional X extensions require updates to the X grammar and the differential corpus;
- once the X compiler becomes authoritative/self-hosting, the X grammar and tests become the normative language definition.

This prevents future contributors from treating accidental reference-lexer behavior as an immutable X language rule.

---

## 7. Sleep tests should cover both early wakeups and excessive latency

### Observation

The current sleep test checks the important lower bound: a requested sleep must not return early. The timer API defines a 1 kHz millisecond tick, making timing behavior testable at useful granularity.

### Recommendation

Add an upper-bound/tolerance check as well. A test should establish that a normal `sleep(50ms)` does not return before 50 ms and does not unexpectedly sleep for a dramatically larger interval.

The exact tolerance should account for interrupt/scheduler behavior, but a bounded regression test will catch a scheduler or wakeup bug that turns a millisecond sleep into a much longer stall.

Also add wraparound tests around the 32-bit timer counter boundary.

---

## 8. Establish the block-device contract before filesystem growth

### Observation

The unified disk registry is already becoming the abstraction underneath multiple storage backends. At the same time, some backends and capabilities are still being brought up incrementally.

This is a good point to freeze the conceptual contract before higher layers begin depending on incidental behavior.

### Recommendation

Define, document, and test:

- sector size;
- LBA width;
- maximum transfer size;
- alignment requirements;
- synchronous vs asynchronous semantics;
- error-code semantics;
- device lifetime/hotplug semantics;
- read/write ordering guarantees;
- whether buffers must be physically contiguous or DMA-capable.

Then make ATA, AHCI, USB storage, and future NVMe implementations conform to that contract.

This will make the eventual VFS/block-cache layer much cleaner.

---

## 9. Error handling and failure semantics

### Observation

As CXK grows, low-level functions are increasingly composed across subsystem boundaries. This makes it important that callers cannot accidentally turn a meaningful failure into an apparently successful operation, or continue operating on partially initialized state.

### Recommendation

Do a dedicated pass for:

- ignored return values from initialization/allocation/device operations;
- error-code collapsing where distinct causes matter to callers;
- cleanup after partially successful initialization;
- functions that modify state before a later operation can fail;
- paths where a failure is logged but execution continues with invalid state.

Where practical, establish a simple rule: once a subsystem operation fails, the caller either handles the failure explicitly or returns it upward. Avoid silent recovery unless the API documents recovery as normal behavior.

---

## 10. Resource ownership and teardown should be explicit

### Observation

Kernel objects are increasingly long-lived: processes, threads, handles, address spaces, endpoint objects, page tables, stacks, timer state, and device registrations all have ownership relationships.

The more concurrency and IPC CXK gains, the easier it becomes for an object to outlive the thing that logically owns it or for a new reference to be taken without a matching release.

### Recommendation

For each persistent kernel object, document:

```text
creator -> owner -> references -> release condition -> final destructor
```

Prefer explicit reference counting for objects that can be referenced asynchronously. Make teardown idempotent where reasonable and ensure failure paths use the same destruction path as normal exits.

This is particularly relevant to IPC endpoints, processes, address spaces, and device objects.

---

## 11. Initialization ordering should be treated as an architectural dependency

### Observation

A growing kernel naturally develops implicit boot ordering: memory must exist before allocators, interrupts before preemption, process state before scheduling, storage before filesystems, and so on.

Implicit ordering works until a subsystem is reused during an earlier phase or initialization becomes asynchronous.

### Recommendation

Document subsystem initialization dependencies explicitly, preferably as a DAG rather than a prose-only boot sequence.

For example:

```text
CPU tables
  -> physical memory
  -> virtual memory
  -> interrupts/timer
  -> scheduler
  -> process/address-space support
  -> devices
  -> block layer
  -> filesystem/VFS
  -> user services
```

Where practical, subsystem APIs should fail cleanly when called before initialization rather than relying on undefined global state.

---

## 12. ABI stability should be planned before the user-space surface expands

### Observation

Syscall structures, handles, capability identifiers, CXEX metadata, and shared kernel/user structures are becoming part of a de facto ABI even while CXK is still under active development.

Changing field sizes, enum values, structure packing, or handle semantics later can become surprisingly expensive once user programs depend on them.

### Recommendation

Mark ABI-facing structures explicitly and document:

- integer widths;
- alignment/packing requirements;
- structure size/version fields where appropriate;
- reserved fields;
- stable enum/ID values;
- handle lifetime semantics;
- syscall error semantics.

Prefer fixed-width types for all on-disk, executable, and user/kernel ABI structures.

---

## 13. x86-32 assumptions should remain localized

### Observation

CXK intentionally targets x86-32 at this stage, so architecture-specific code is expected. The engineering concern is allowing those assumptions to leak into supposedly generic interfaces.

Common long-term trouble spots include pointer-to-integer casts, implicit integer-width assumptions, packed structures, alignment, page-size constants, endianness, and device addressing widths.

### Recommendation

Keep architecture-dependent definitions behind clearly named interfaces or headers. For generic code, prefer:

- `uintptr_t` for pointer-sized integers;
- fixed-width integers for serialized formats;
- explicit alignment/packing declarations for ABI data;
- named architecture constants for page/table widths;
- helper functions for address conversion rather than scattered casts.

This will make a future architecture port, even if not currently planned, dramatically easier and will also expose accidental width bugs on the current target.

---

## 14. Fatal-path behavior should distinguish recoverable subsystem failure from kernel invariants

### Observation

A low-level kernel necessarily has operations that cannot safely continue after failure. At the same time, device errors, allocation failures, malformed user requests, and unavailable optional services should not automatically become system-wide fatal errors.

### Recommendation

Define which failures are:

```text
recoverable request failure
process-fatal failure
subsystem-fatal failure
kernel-fatal invariant violation
```

Use assertions/panics for violated internal invariants and normal error returns for expected environmental/request failures.

This distinction will become increasingly important once user services and storage failures are expected to occur without taking down the whole kernel.

---

## 15. Dead and transitional code should be periodically retired

### Observation

CXK is evolving rapidly, so older scheduler, memory-management, process, and boot paths can remain useful as scaffolding even after a newer path becomes authoritative.

The danger is not merely code size: duplicated paths make it unclear which behavior is actually supported and can cause fixes to be applied to one path but not another.

### Recommendation

Periodically classify transitional code as one of:

- active production path;
- temporary compatibility path;
- test-only path;
- planned replacement;
- obsolete and removable.

Add explicit comments or tracking issues for temporary paths and remove them once their replacement is proven.

---

## 16. Test the public lifecycle, not only individual helpers

### Observation

Unit-style tests are valuable, but many kernel bugs occur between individually-correct operations: create → run → block → wake → exit, map → unmap → remap, open → close → process exit, device registration → I/O → teardown, and so on.

### Recommendation

Continue the existing kernel self-test approach, but add lifecycle tests that exercise complete subsystem sequences through their public interfaces.

Examples:

```text
process create -> user entry -> syscall -> exit -> cleanup
endpoint create -> IPC call -> reply -> close -> reclaim
map -> access -> unmap -> access failure
sleep -> timer wake -> reschedule
block device register -> read/write -> unregister
```

These tests should be especially valuable before enabling concurrency or SMP.

---

## 17. Recommended engineering backlog

### P1 — subsystem contracts

- [ ] Define the generic block-device contract.
- [ ] Remove silent ATA LBA/count truncation.
- [ ] Formalize scheduler mode/lifecycle transitions.
- [ ] Make per-process `esp0` a documented prerequisite for concurrent ring-3 execution.
- [ ] Define ownership/lifetime rules for persistent kernel objects.
- [ ] Document initialization dependencies.

### P2 — robustness and tests

- [ ] Harden X lexer lookahead/range arithmetic.
- [ ] Standardize bounded-string API contracts.
- [ ] Add upper-bound timing tests for sleep.
- [ ] Add timer-counter wraparound tests.
- [ ] Audit ignored return values and partial-initialization cleanup.
- [ ] Add lifecycle-oriented subsystem tests.

### P3 — architecture hygiene

- [ ] Document the authoritative/reference relationship between the X lexer and its compatibility lexer.
- [ ] Freeze the block-device API before substantial filesystem/VFS layering.
- [ ] Document backend-specific capabilities and maximum transfer sizes.
- [ ] Define the migration path from cooperative scheduling to preemptive single-CPU execution, concurrent user threads, and SMP.
- [ ] Mark and version user/kernel ABI structures.
- [ ] Localize x86-specific assumptions.
- [ ] Periodically remove obsolete transitional paths.
- [ ] Define fatal vs recoverable failure semantics.

---

## Conclusion

The non-security findings are mostly signs of a kernel entering a more mature phase rather than evidence of fundamental design problems. The most valuable work now is making subsystem boundaries explicit before additional layers build on them.

In particular, storage and scheduling are becoming foundational interfaces. Stabilizing those contracts now should reduce the amount of compatibility and refactoring work required when CXK grows into a more fully concurrent operating system.

The additional lifecycle, ownership, ABI, initialization, and failure-semantics work is best treated as architectural hygiene rather than a reason to halt development. Doing it incrementally alongside subsystem growth should keep the codebase understandable as CXK moves toward concurrency, richer storage, and a larger user-space ABI.
