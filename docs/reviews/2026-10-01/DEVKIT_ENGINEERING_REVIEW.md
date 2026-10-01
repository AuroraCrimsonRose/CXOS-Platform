# CX DevKit Engineering Review

**Scope:** non-security engineering and architecture observations for CX DevKit  
**Focus:** parsing, artifact generation, ABI management, tool execution, testing, layering, maintainability, and build-system contracts  
**Excluded:** security findings covered by `docs/SECURITY_REVIEW.md`  
**Review date:** 2026-10-01

> **Response & status:** every finding below has been checked against the code
> and planned in [`docs/planning/HARDENING_PLAN.md`](../../planning/HARDENING_PLAN.md), which tracks each to
> done. This review is kept as written.

## Executive summary

CX DevKit is moving from a collection of command-line/build utilities toward a reusable toolchain and IDE-facing platform. The main engineering concern is therefore not that the architecture is fundamentally wrong; it is that several subsystem contracts are becoming important enough to formalize before more consumers depend on them.

The highest-value engineering work is:

1. Harden and formalize ELF parsing as a reusable validated representation.
2. Make CXEX layout calculations checked and explicit.
3. Establish a clear parse → validate → transform → emit pipeline.
4. Add a real .NET test project alongside the existing external/differential tests.
5. Resolve the documented artifact-format taxonomy versus the actual ELF → CXEX pipeline.
6. Finish separating shared tool execution logic from CLI presentation.
7. Reduce duplicated ABI definitions and establish a canonical ABI source.
8. Make library/CLI/Studio layering an explicit architectural rule.

---

## 1. ELF parser should provide a validated representation

### Observation

The ELF parser is currently primarily concerned with extracting the information required by the conversion pipeline. As the DevKit grows, downstream consumers would benefit from being able to assume that a successfully parsed ELF is structurally coherent.

### Recommendation

Make parsing a two-stage operation conceptually:

```text
raw ELF
  ↓
parse
  ↓
structural validation
  ↓
validated ELF model
```

The validated model should establish file bounds, header sizes, program-header consistency, segment relationships, and other format invariants once rather than requiring each consumer to repeat them.

This also provides a clean boundary between ELF-specific logic and CXEX-specific policy.

---

## 2. CXEX layout should be a first-class intermediate model

### Observation

CXEX generation currently combines section metadata, offsets, payload sizes, virtual addresses, and output serialization in close proximity.

That is workable while the format is small, but it becomes harder to reason about as additional metadata, signing, permissions, or versions are added.

### Recommendation

Introduce a conceptual layout phase:

```text
input model
   ↓
CXEX layout model
   ↓
validated layout
   ↓
canonical serializer
```

The layout model should contain already-resolved offsets, sizes, virtual ranges, and section relationships. Serialization should primarily turn that validated model into bytes rather than making architectural decisions while writing the file.

---

## 3. Establish a consistent parse/validate/emit pipeline

The major format-processing components should follow a consistent lifecycle:

```text
parse
  ↓
structural validation
  ↓
semantic validation
  ↓
target-policy validation
  ↓
transform/layout
  ↓
canonical serialization
  ↓
optional signing
```

This keeps file-format correctness separate from target-specific policy and makes later testing substantially easier.

It also prevents individual writers and converters from accumulating their own subtly different validation rules.

---

## 4. Add a first-class .NET test project

### Observation

The repository already has valuable external tests, language tests, compiler tests, and ABI/differential validation. Those are excellent integration-level protections, but they do not replace direct unit tests of the C# libraries.

### Recommendation

Add a test project covering the shared libraries, particularly:

- `CXEX.Build`;
- `CXEX.Crypto`;
- `CXEX.Disk`;
- `CXEX.FileSystem`;
- `CXEX.Lang`;
- `CXEX.Core`.

Direct library tests should cover both normal and malformed inputs without needing to launch the entire CLI.

The existing external test suite should remain; the two levels serve different purposes.

---

## 5. Resolve the artifact-format taxonomy

### Observation

The conceptual format documentation describes an intermediate `.XCXN` stage, while the current implementation uses a pipeline closer to:

```text
.XFXN
  ↓
assembly
  ↓
object
  ↓
ELF
  ↓
.XUEX
```

If the intermediate format is no longer planned, keeping it in the architecture documentation creates unnecessary ambiguity.

### Recommendation

Update the format taxonomy to match the implementation, or explicitly identify `.XCXN` as a future/optional stage if it remains part of the roadmap.

The documentation should make it immediately clear which formats are currently implemented and which are conceptual/future formats.

---

## 6. Extract shared tool execution into a stable abstraction

### Observation

The current process runner is intentionally simple and appropriate for the present CLI. However, both the CLI and future Studio tooling will need to execute compilers, linkers, assemblers, and other external tools.

### Recommendation

Keep process execution in shared library/tooling code rather than allowing Studio to depend on CLI implementation details.

A future abstraction could conceptually expose:

```text
ToolSpec
ProcessOptions
ToolResult
IProcessRunner
```

with support for:

- executable discovery;
- working directory;
- environment variables;
- stdout/stderr capture;
- exit status;
- cancellation;
- timeout;
- tool version information.

The CLI should remain responsible for presentation, command parsing, and user-facing formatting.

---

## 7. ABI duplication should converge on a canonical source

### Observation

`AbiSync` is a strong mitigation for keeping ABI declarations synchronized, but the underlying model still has multiple representations of ABI information, including C headers and X-side definitions.

### Recommendation

Long term, consider an intermediate ABI description generated or extracted from the authoritative ABI source:

```text
canonical ABI
    ↓
ABI model
 ├── C declarations
 ├── X declarations
 ├── C# constants/structures
 ├── ABI tests
 └── documentation
```

This is not necessarily urgent while `AbiSync` is effective, but it would reduce duplicated semantic ownership as the ABI expands.

---

## 8. Make library/CLI/Studio layering explicit

The intended architecture is strongest when reusable behavior lives in libraries and both CLI and Studio act as front ends.

Recommended dependency direction:

```text
CXEX libraries
    ↓
CLI / Studio
```

Avoid making:

```text
Studio → CLI implementation
Library → CLI implementation
```

This will make Studio development significantly easier because it can consume the same build, compiler, ABI, filesystem, and artifact APIs directly.

---

## 9. Toolchain versions should be explicit

As the DevKit invokes GCC, assemblers, linkers, and other external tools, build results increasingly depend on the exact toolchain versions involved.

### Recommendation

Represent toolchain identity explicitly where useful:

- compiler version;
- assembler version;
- linker version;
- target triple/configuration;
- DevKit version;
- relevant feature flags.

This information should be available to diagnostics and, where appropriate, recorded in reproducible build metadata rather than inferred from arbitrary host state.

---

## 10. Error handling should preserve subsystem boundaries

Build systems often become difficult to debug when a low-level error is converted into a generic "build failed" result too early.

Preserve meaningful distinctions between:

```text
input error
parser error
validation error
compiler failure
linker failure
filesystem failure
artifact-layout failure
signing failure
```

The CLI can collapse these into user-friendly output while the library layer retains structured error information.

Prefer structured result/error types where multiple consumers need to react differently to the same failure.

---

## 11. Artifact generation should be deterministic where possible

Deterministic output is useful for testing, caching, debugging, and eventually reproducible builds.

Define treatment of:

- section ordering;
- padding;
- timestamps;
- metadata ordering;
- generated identifiers;
- embedded paths;
- toolchain metadata.

If identical source/toolchain/configuration inputs are expected to produce identical CXEX bytes, make that a documented invariant and test it.

---

## 12. File and directory APIs should have explicit lifecycle semantics

The DevKit contains components that operate on disk images, filesystems, generated artifacts, and build outputs. Their APIs should make ownership and lifecycle obvious.

Document where appropriate:

```text
create/open
  ↓
use
  ↓
flush
  ↓
close/dispose
```

For mutable image/file objects, explicitly define whether methods mutate in memory, immediately persist, or require an explicit commit/flush operation.

This becomes particularly important once Studio introduces long-lived project sessions.

---

## 13. Version and format compatibility should be centralized

As CXEX, X data, ABI structures, and toolchain metadata gain versions, avoid scattering compatibility checks across unrelated commands.

Prefer central version/policy components that answer questions such as:

- Is this artifact format supported?
- Can this DevKit read this version?
- Can this target consume this artifact?
- Is an ABI version compatible?
- Can this toolchain produce the requested target format?

This keeps CLI behavior and Studio behavior consistent.

---

## 14. Transitional code should be clearly classified

CX DevKit is evolving rapidly, so temporary wrappers, compatibility paths, and migration code are expected.

The engineering risk is allowing temporary code to become an undocumented second implementation.

Periodically classify transitional code as:

- active path;
- compatibility path;
- test-only path;
- planned replacement;
- obsolete/removable.

Add a short comment or tracking reference to intentional transitional code and remove it once the replacement is established.

---

## 15. Recommended engineering backlog

### P1 — contracts and correctness

- [ ] Make ELF parsing produce a validated representation.
- [ ] Make CXEX layout a first-class intermediate model.
- [ ] Establish parse → validate → transform → emit as the common pipeline.
- [ ] Resolve the implemented-vs-documented artifact taxonomy.
- [ ] Add direct .NET tests for core libraries.

### P2 — architecture and maintainability

- [ ] Extract shared tool execution into reusable infrastructure.
- [ ] Preserve strict library → CLI/Studio layering.
- [ ] Centralize version/format compatibility.
- [ ] Preserve structured error information across library boundaries.
- [ ] Define file/image lifecycle semantics.

### P3 — long-term toolchain maturity

- [ ] Establish a canonical ABI source/intermediate representation.
- [ ] Make artifact generation deterministic.
- [ ] Record toolchain identity where useful for reproducibility.
- [ ] Periodically retire obsolete transitional paths.

---

## Conclusion

The DevKit's architecture is fundamentally well suited to becoming the shared build/toolchain layer for CXK and CXOS. The main engineering task now is to make the boundaries explicit: validated input models, explicit artifact layouts, structured errors, stable tool execution, canonical ABI ownership, and strict library/CLI/Studio separation.

Doing this incrementally will make the transition from command-line tooling to a larger IDE/build platform much less disruptive.
