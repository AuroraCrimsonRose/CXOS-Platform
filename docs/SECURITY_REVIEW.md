# CXK Security Review

**Review scope:** kernel/runtime security-sensitive code on `x86_32_DEV`  
**Review focus:** zero-day prevention, privilege boundaries, memory isolation, executable loading, syscall boundaries, IPC, and resource exhaustion  
**Excluded:** development tooling, compiler/editor tooling, and other non-runtime components unless required to understand a security boundary  
**Review date:** 2026-10-01

> **Response & status:** every finding below has been checked against the code
> and planned in [`HARDENING_PLAN.md`](HARDENING_PLAN.md), which tracks each to
> done. This review is kept as written.

## Executive summary

CXK has a substantial security architecture already in place: per-process address spaces, capability attenuation, guarded kernel stacks, signed executable verification, user-pointer validation, process teardown, and ring-3-oriented self-tests. These are good foundations for a capability-oriented protected kernel.

The current implementation should **not yet be described as hardened or zero-day resistant**. The largest remaining risks are concentrated around the CXEX executable loader and its interaction with the virtual-memory subsystem. In particular, executable-provided virtual addresses are not sufficiently constrained to the user address range before being mapped, and several untrusted offset/size calculations are susceptible to integer-wrap mistakes. There are also weaker issues around writable user pointers, W^X consistency, IPC buffer lifetime, and endpoint exhaustion.

The priority order recommended by this review is:

1. Enforce user/kernel virtual-address separation in CXEX loading.
2. Make all CXEX offset/size/range calculations overflow-safe.
3. Validate the complete executable image before allocating or mapping pages.
4. Account executable image memory against the process memory quota.
5. Distinguish readable and writable user pointers at syscall boundaries.
6. Enforce W^X consistently for executable images.
7. Close the IPC buffer lifetime/TOCTOU gap.
8. Reclaim IPC endpoint objects and prevent global endpoint exhaustion.

---

## 1. Critical: CXEX loader can target the kernel virtual-address range

### Problem

The CXEX loader accepts section virtual addresses from the executable and eventually maps those addresses through the paging layer. The loader does not currently establish the invariant that a user executable section lies entirely below the kernel virtual-address base.

The paging primitive itself is intentionally powerful: it can create user mappings and propagate PDE flags. Consequently, an insufficiently constrained caller can turn an executable-controlled virtual address into a mapping in the kernel half of the address space.

### Security impact

A maliciously constructed executable could attempt to map a user-accessible page at or above the kernel virtual-address boundary. If successful, this can undermine the kernel/user isolation invariant and potentially enable corruption or disclosure of kernel memory.

This is the highest-priority finding because it crosses the fundamental ring-3/ring-0 address-space boundary.

### Required fix

Before allocating or mapping any CXEX section, enforce an overflow-safe range check equivalent to:

```c
section_va >= USER_MIN
section_va + mem_size <= KERNEL_VBASE
```

The addition must be performed using subtraction-based bounds checking rather than an overflowing integer addition.

Also reject mappings that overlap existing protected regions where the executable format does not explicitly permit them.

### Recommended architectural hardening

Consider separating the unrestricted internal paging primitive from security-policy-aware primitives, for example:

```c
paging_map_kernel(...)
paging_map_user(...)
paging_map_user_image(...)
```

The user/image variants can mechanically enforce the user virtual-address range, physical-frame validity, user permissions, and W^X policy instead of relying on every caller to reproduce the rules correctly.

---

## 2. High: CXEX signature-offset parsing is vulnerable to integer-wrap validation

### Problem

The signature parser performs a bounds check conceptually equivalent to:

```c
if (signature_offset + signature_header_size > file_length)
    reject;
```

On a 32-bit target, an attacker-controlled offset can overflow the addition before the comparison.

### Security impact

The signature parser processes untrusted executable bytes before the executable is trusted. A malformed CXEX image could therefore cause the verifier to construct an invalid pointer or read outside the supplied image buffer.

This is a kernel memory-safety issue in the executable trust boundary.

### Required fix

Use subtraction-based bounds checks throughout the CXEX parser:

```c
if (offset > len)
    return ERROR;
if (size > len - offset)
    return ERROR;
```

Apply the same rule to every offset/length pair in the format parser, not just the signature block.

---

## 3. High: CXEX loader has additional integer-wrap and resource-exhaustion paths

### Problem

Executable loading performs calculations involving virtual addresses, file sizes, memory sizes, and page endpoints. Calculations equivalent to `virt_addr + size` and page-end arithmetic must be treated as attacker-controlled arithmetic.

The loader also allocates executable pages directly rather than clearly applying the process virtual-memory quota used by ordinary user mappings.

### Security impact

A malformed or hostile executable could cause:

- wrapped virtual ranges;
- incomplete or unexpected mappings;
- excessive page allocation;
- physical-memory exhaustion;
- partial image construction followed by an error.

Even if the executable must be signed, this remains relevant whenever an untrusted image can reach the parser/verifier, and it also protects against mistakes or compromised signing infrastructure.

### Required fix

Use a two-phase loader:

1. Parse every section.
2. Validate every file range.
3. Validate every virtual range.
4. Reject overlapping sections unless explicitly supported.
5. Calculate total page consumption using overflow-safe arithmetic.
6. Compare the result against a hard image-memory limit and the process quota.
7. Only after all checks succeed, allocate and map pages.

This prevents malformed images from partially mutating the address space before the loader discovers an error.

---

## 4. Medium/High: `user_ptr_ok()` does not establish writability

### Problem

The user-pointer validation path verifies that a range is a present user mapping, but it does not establish that the mapping is writable.

This matters for syscalls where the kernel writes results into a user-provided buffer. A read-only user page can pass the generic pointer check even though the syscall intends to write through it.

### Security impact

This is primarily a protection-model weakness and a potential denial-of-service/crash path. It also makes the syscall ABI easier to misuse as additional concurrent memory-management features are added.

### Required fix

Split pointer validation by access direction:

```c
user_ptr_readable(ptr, len)
user_ptr_writable(ptr, len)
```

Use the writable form for every kernel-to-user output buffer and the readable form for every user-to-kernel input buffer.

For bidirectional structures, explicitly validate the required access for each phase rather than relying on one generic predicate.

---

## 5. Medium: W^X policy is inconsistent between `vm_map()` and CXEX loading

### Problem

The ordinary virtual-memory mapping path rejects simultaneous write and execute permissions. CXEX section loading does not appear to enforce the same invariant consistently.

On 32-bit x86 without NX support, an executable writable mapping is effectively executable regardless of a software-level execute flag.

### Security impact

A writable-and-executable section weakens exploit mitigations and creates an unnecessary primitive for self-modifying or injected code.

### Required fix

Apply one W^X policy across all user mappings, including executable image loading. Unless there is an explicit and documented exception, reject any CXEX section requesting both write and execute permissions.

If a future feature requires JIT/self-modifying code, make that a separate capability-controlled mechanism rather than weakening the default loader policy.

---

## 6. Medium: IPC reply buffer lifetime / TOCTOU risk

### Problem

IPC validates a reply buffer and stores the user pointer while the caller blocks. When the caller resumes, the kernel copies the reply using the stored user pointer.

As CXK evolves toward more concurrent process/thread behavior, the mapping can potentially change between validation and use.

### Security impact

This is a classic user-memory TOCTOU condition. The immediate exploitability is limited by the current process/address-space model, but it becomes more significant with shared address spaces and concurrent user threads.

### Recommended fix

Prefer copying IPC reply data into kernel-owned storage for the duration of the transaction and performing a fresh user-access validation at the final copy-out point.

For operations that must retain user pages for a longer period, introduce explicit page pinning rather than retaining unchecked user pointers.

---

## 7. Medium: IPC endpoint exhaustion

### Problem

IPC endpoints use a fixed global pool. Endpoint creation consumes an entry, while the current close/release path does not appear to return endpoint objects to the pool.

### Security impact

A process with permission to create endpoints can eventually consume the global endpoint table and deny endpoint creation to other processes.

### Required fix

Introduce explicit endpoint reference counting and reclamation. Endpoint destruction should occur only after no process handles or active IPC operations reference the endpoint.

Also consider per-process endpoint quotas in addition to the global capacity limit.

---

## 8. Architectural hardening: make VM security invariants difficult to violate

The current paging API is intentionally low-level, which is appropriate for a kernel, but it means security properties depend heavily on every caller performing the correct checks.

The most important invariants should be represented in the API itself where practical:

- user mappings cannot enter the kernel virtual-address range;
- user mappings cannot expose invalid physical frames;
- user mappings cannot modify kernel-half PDEs;
- executable mappings obey W^X;
- user writable/readable permissions are explicit;
- executable image mappings are subject to process resource limits.

A security-sensitive kernel benefits from making the unsafe operation difficult to express rather than merely documenting that callers must use it carefully.

---

## 9. Existing security controls that are already valuable

The review also found several strong foundations that should be preserved.

### Signed executable verification

CXK distinguishes executable integrity/trust from publisher trust rather than treating any syntactically valid image as trusted. The executable verification path checks the embedded key material and its trust relationship before execution.

### Capability attenuation

Spawned processes receive capabilities derived from the caller rather than arbitrary caller-selected privilege. This prevents a child process from simply requesting capabilities its parent does not possess.

### Per-process address spaces

Process creation establishes independent address spaces and CR3 state. This is a critical foundation for user/kernel and process/process isolation.

### Guarded kernel stacks

Kernel stacks now use protected stack regions rather than ordinary heap allocations, and the double-fault path has its own task-gate stack. This turns stack exhaustion from silent memory corruption into a detectable kernel fault.

### Process/thread startup ordering

Process threads are established with their address space, kernel stack, process metadata, and identity before becoming runnable. This avoids exposing partially initialized process state to scheduling/preemption.

### User-style syscall testing

The filesystem self-tests exercise the actual syscall path with user mappings and capability checks instead of calling kernel helpers directly. This is an excellent pattern for future security regression tests.

### Zeroing newly exposed user memory

New anonymous user pages are cleared before being made visible to user code, preventing stale physical-memory contents from becoming a cross-process information disclosure primitive.

---

## 10. Recommended security test plan

The existing kernel self-test infrastructure should be extended with adversarial cases rather than relying exclusively on happy-path tests.

### CXEX parser tests

- offset = `UINT32_MAX`;
- offset + size wrapping;
- size = `UINT32_MAX`;
- zero-length sections;
- section extending exactly to EOF;
- section extending one byte past EOF;
- overlapping sections;
- overlapping file and memory ranges;
- virtual-address addition wrapping;
- page-end addition wrapping;
- absurd section counts;
- absurd total image size;
- malformed signature blocks;
- malformed key/fingerprint lengths.

### VM isolation tests

Attempt to load/map sections at:

```text
KERNEL_VBASE - 1
KERNEL_VBASE
KERNEL_VBASE + 1
UINT32_MAX - page_size
```

Verify that all are rejected when they cross or enter the kernel range.

Also verify that a failed executable load leaves the process address space unchanged.

### Pointer validation tests

For every syscall that writes to userspace, test:

- valid writable user page;
- valid read-only user page;
- kernel-half pointer;
- unmapped pointer;
- range ending exactly at the page boundary;
- range crossing from a valid page into an unmapped page;
- integer-overflowing `ptr + len`.

### Resource-exhaustion tests

Attempt to exhaust:

- physical frames through executable loading;
- process virtual-memory quota;
- endpoint table entries;
- process/thread slots;
- handle slots.

The expected result should be a controlled failure, not corruption, a kernel panic, or starvation of unrelated processes.

### IPC tests

Test endpoint close while an IPC operation is pending, repeated create/close cycles, invalidated mappings, and concurrent address-space changes once multithreading permits them.

---

## 11. Security priority summary

| Priority | Area | Issue |
|---|---|---|
| Critical | CXEX / VM | User-controlled executable VA can reach kernel address range |
| High | CXEX parser | Integer-wrapable signature bounds validation |
| High | CXEX loader | Integer-wrap/resource-exhaustion paths |
| Medium/High | Syscalls | Pointer validation does not establish writability |
| Medium | CXEX / VM | W^X policy is inconsistent |
| Medium | IPC | Stored user pointer can become stale between validation and use |
| Medium | IPC | Endpoint pool exhaustion/reclamation |

These priorities describe security impact and remediation urgency; they are not a claim that any finding is currently exploitable on every CXK configuration.

---

## Conclusion

CXK already has the shape of a security-conscious protected kernel rather than a flat hobby kernel with security added afterward. The capability model, address-space separation, executable signing, guarded stacks, process initialization ordering, and ring-3-oriented testing are all strong foundations.

The current hardening gap is concentrated at the boundary where **untrusted executable metadata becomes privileged VM operations**. Fixing that boundary first, then systematically making arithmetic and user-memory access overflow-safe and direction-aware, will remove the most important classes of kernel memory-corruption and isolation failures identified in this review.

Until those issues are addressed and covered by regression tests, CXK should be described as having a **security architecture under active hardening**, rather than as hardened or zero-day resistant.
