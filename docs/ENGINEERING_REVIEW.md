# CXK Engineering Review

**Review scope:** non-security engineering and architecture observations from the `x86_32_DEV` kernel/runtime review  
**Focus:** correctness, subsystem contracts, maintainability, scheduler architecture, storage, compiler tooling, and test coverage  
**Excluded:** security findings covered by `docs/SECURITY_REVIEW.md`  
**Review date:** 2026-10-01

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

## 9. Recommended engineering backlog

### P1 — subsystem contracts

- [ ] Define the generic block-device contract.
- [ ] Remove silent ATA LBA/count truncation.
- [ ] Formalize scheduler mode/lifecycle transitions.
- [ ] Make per-process `esp0` a documented prerequisite for concurrent ring-3 execution.

### P2 — robustness and tests

- [ ] Harden X lexer lookahead/range arithmetic.
- [ ] Standardize bounded-string API contracts.
- [ ] Add upper-bound timing tests for sleep.
- [ ] Add timer-counter wraparound tests.

### P3 — architecture hygiene

- [ ] Document the authoritative/reference relationship between the X lexer and its compatibility lexer.
- [ ] Freeze the block-device API before substantial filesystem/VFS layering.
- [ ] Document backend-specific capabilities and maximum transfer sizes.
- [ ] Define the migration path from cooperative scheduling to preemptive single-CPU execution, concurrent user threads, and SMP.

---

## Conclusion

The non-security findings are mostly signs of a kernel entering a more mature phase rather than evidence of fundamental design problems. The most valuable work now is making subsystem boundaries explicit before additional layers build on them.

In particular, storage and scheduling are becoming foundational interfaces. Stabilizing those contracts now should reduce the amount of compatibility and refactoring work required when CXK grows into a more fully concurrent operating system.
