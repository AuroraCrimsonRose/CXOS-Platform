# CX DevKit Security Review

**Scope:** CX DevKit build, parsing, artifact generation, signing, trust, and tool-execution boundaries  
**Focus:** malformed input, executable-format validation, cryptographic trust, signing boundaries, filesystem/tooling behavior, and supply-chain integrity  
**Excluded:** general engineering observations covered by `docs/ENGINEERING_REVIEW.md`  
**Review date:** 2026-10-01

> **Response & status:** every finding below has been checked against the code
> and planned in [`HARDENING_PLAN.md`](HARDENING_PLAN.md), which tracks each to
> done. This review is kept as written.

## Executive summary

CX DevKit sits on an unusually important security boundary: it transforms compiler output and other build inputs into CXEX artifacts that CXK can execute and potentially trust.

The DevKit should therefore be treated as security-sensitive build infrastructure even though most of its code runs outside the kernel. A bug in the DevKit must not become a way to produce an artifact that bypasses the kernel's own validation, and the DevKit should independently reject malformed or ambiguous inputs before signing them.

The highest-priority areas are:

1. Make ELF parsing fully bounds-checked and structurally validating.
2. Make CXEX layout and size arithmetic checked and overflow-safe.
3. Define complete and canonical signature coverage.
4. Strictly separate platform/root, publisher, and artifact trust authorities.
5. Keep Secure Boot trust distinct from CXEX publisher trust.
6. Harden private-key handling and signing workflows.
7. Treat filesystem paths and external tool execution as explicit trust boundaries.
8. Build toward deterministic/reproducible artifact generation and supply-chain verification.

---

## 1. High: ELF parsing must be a validating parser

### Problem

ELF is an input format and therefore untrusted data from the DevKit's perspective. Parsing should not merely extract the fields currently needed by the build pipeline; it should establish that the complete ELF structure is internally consistent before downstream code consumes it.

### Required validation

At minimum, validate:

- ELF magic;
- class and endianness;
- supported ELF version;
- header size;
- program-header entry size;
- program-header table bounds;
- section-header table bounds when sections are consumed;
- `p_offset + p_filesz` without integer overflow;
- `p_filesz <= p_memsz` for loadable segments;
- segment alignment requirements;
- segment count against a sane limit;
- total loadable memory against a sane limit;
- overlapping or contradictory loadable segments;
- entry-point placement where the target format requires it.

Use checked arithmetic or subtraction-based bounds checks. Never allow malformed offsets to reach array slicing or file reads first.

### Architectural goal

`ElfParser.Parse()` should return an ELF representation that is already structurally valid. Downstream layout and conversion code should not need to repeat low-level file-bound checks.

---

## 2. High: CXEX writer/layout arithmetic must be checked

### Problem

CXEX generation combines section counts, offsets, file sizes, memory sizes, and virtual addresses. These values eventually control managed allocations and serialized offsets.

Expressions equivalent to:

```csharp
headerSize + sectionCount * sectionSize + totalPayload
fileOffset + fileSize
virtualAddress + memorySize
```

must be treated as potentially overflowing arithmetic.

### Required fix

Perform layout calculations using checked arithmetic and sufficiently wide intermediate types. Reject any result that cannot be represented by the CXEX format or the output buffer type.

Do not rely on a later `byte[]` allocation or integer conversion to detect invalid layouts.

### Additional validation

The writer should reject:

- overlapping file ranges;
- overlapping incompatible virtual ranges;
- impossible memory/file size relationships;
- unreasonable section counts;
- unreasonable aggregate image size;
- offsets outside the output image;
- arithmetic wraparound.

---

## 3. Critical architectural invariant: signing must cover the entire security-relevant artifact

A CXEX signature is only meaningful if every byte that can affect the kernel's interpretation of the executable is authenticated.

The signed representation should therefore include, as applicable:

- CXEX header fields;
- version/format identifiers;
- entry point;
- section metadata;
- virtual addresses;
- permissions;
- file offsets and sizes;
- memory sizes;
- payload bytes;
- authority/key identifiers;
- security-relevant flags.

Do not sign only payload data while leaving executable metadata mutable outside the authenticated representation.

### Canonicalization

Define exactly one canonical byte representation for signing and verification. Specify behavior for:

- section ordering;
- padding;
- reserved fields;
- unused bytes;
- integer encoding;
- alignment;
- optional metadata.

Two tools must not be able to produce different security interpretations of logically equivalent artifacts without the signature mechanism detecting the difference.

---

## 4. High: Separate trust authorities and privilege domains

CX DevKit contains multiple conceptual trust levels, including platform/root authority, publisher authority, and artifact-level signing.

These must remain separate security domains.

A publisher key must not accidentally become capable of signing platform-authoritative artifacts merely because the same cryptographic implementation can process both.

Define explicit authority types and enforce them in the artifact metadata and verifier.

A useful conceptual model is:

```text
platform authority
      ↓
authorized publisher / artifact authority
      ↓
specific executable artifact
```

The exact hierarchy may differ by CXK design, but the authority transitions should be explicit rather than inferred from a key's mere presence.

---

## 5. High: Secure Boot trust must remain distinct from CXEX publisher trust

Secure Boot and CXEX signing can participate in one overall chain of trust, but they represent different trust domains.

A key trusted to authenticate firmware or boot components should not automatically become a key authorized to sign arbitrary user executables.

The DevKit should encode and preserve this distinction, while CXK independently enforces it when consuming artifacts.

---

## 6. High: cryptographic policy must be explicit

The crypto layer should reject algorithms and parameters outside the project's defined security policy rather than accepting whatever the underlying framework can technically parse.

Document and enforce:

- minimum RSA key size;
- permitted hash algorithms;
- permitted signature schemes/encodings;
- key identifier/fingerprint format;
- malformed key handling;
- unsupported algorithm identifiers;
- signature length expectations;
- authority/type matching.

Avoid algorithm agility that silently expands the accepted trust surface. New algorithms should be explicitly enabled by policy and covered by compatibility tests.

---

## 7. High: private signing keys require a dedicated handling policy

Private signing keys should never be treated like ordinary build artifacts.

Audit all key generation/import/export/signing paths for:

- plaintext persistence;
- accidental inclusion in output directories;
- command-line argument exposure;
- verbose logging;
- exception messages containing key material;
- temporary-file leakage;
- insecure default locations;
- unintended source-control inclusion;
- overly broad filesystem permissions.

Where possible, signing APIs should accept key material through protected handles or secure stores rather than repeatedly materializing private key bytes.

The CLI should make the distinction between public verification material and private signing material obvious.

---

## 8. Medium/High: filesystem paths are a security boundary

Build tooling frequently accepts paths supplied by projects, CI systems, IDEs, or other automation. Treat those paths as untrusted input when the DevKit runs in a context with meaningful filesystem privileges.

Review input/output path handling for:

- input/output path collisions;
- unintended overwrite of source artifacts;
- path traversal where paths are resolved relative to an output root;
- symlink/reparse-point behavior;
- predictable temporary filenames;
- temporary-file cleanup;
- partially written files after failure;
- permissions on generated artifacts.

Where atomic output matters, write to a secure temporary file and atomically replace the destination only after successful generation and signing.

---

## 9. Medium/High: external tool execution is a trust boundary

The DevKit invokes external compiler/linker/build tools. Tool execution should not rely on ambiguous shell parsing or inherited process state.

Prefer direct process invocation with explicitly separated executable and argument fields. Define:

- executable discovery rules;
- working-directory semantics;
- environment-variable inheritance;
- timeout/cancellation behavior;
- stdout/stderr handling;
- exit-code interpretation;
- tool-version validation.

Do not silently fall back to a different compiler/linker version when reproducibility or artifact trust matters.

---

## 10. Medium: prevent malformed artifacts from becoming signed artifacts

The signing stage should occur only after all structural and semantic validation succeeds.

Recommended pipeline:

```text
untrusted input
    ↓
parse
    ↓
structural validation
    ↓
semantic validation
    ↓
target/CXEX policy validation
    ↓
canonical serialization
    ↓
sign
    ↓
final artifact
```

Signing should be a finalization operation, not a validation substitute.

If any post-signing transformation can alter security-relevant bytes, the artifact must be considered unsigned until it is signed again.

---

## 11. Medium: DevKit and CXK must independently validate CXEX

The DevKit should produce well-formed CXEX files, but CXK must never assume that a DevKit-produced artifact is safe merely because it carries a valid signature.

This is intentional defense in depth:

```text
CX DevKit
  └─ validates and emits
       ↓
     signed CXEX
       ↓
CXK
  └─ independently validates before execution
```

A DevKit bug, compromised build environment, or compromised signing key must not eliminate the kernel's executable-format and memory-policy checks.

---

## 12. Medium: deterministic artifact generation

For signed software, reproducibility is a security property as well as a build-quality feature.

Where practical, CXEX generation should be deterministic for identical inputs and configuration. Define treatment of:

- timestamps;
- random identifiers;
- section ordering;
- padding;
- metadata ordering;
- toolchain version information;
- path information.

A future verification tool should be able to reproduce the unsigned canonical representation independently and compare hashes before signing.

---

## 13. Medium/Long-term: supply-chain integrity

As CX DevKit becomes the toolchain responsible for producing trusted CXK artifacts, the toolchain itself becomes part of the trust chain.

Longer-term controls should include:

- pinned dependencies;
- controlled compiler/linker versions;
- reproducible DevKit builds;
- artifact provenance;
- checksums or signatures for downloaded toolchains;
- independently verifiable releases;
- documented build environment requirements.

The goal is not to make the development environment impossible to use, but to make it possible to establish where a trusted CXEX artifact came from and how it was produced.

---

## 14. Security regression tests

Add adversarial tests covering at least:

### ELF

- truncated ELF headers;
- invalid program-header sizes;
- program-header table outside the file;
- `offset + size` overflow;
- `p_filesz > p_memsz`;
- overlapping loadable segments;
- absurd segment counts;
- absurd aggregate memory size;
- invalid alignment;
- invalid entry point.

### CXEX

- section-count overflow;
- file-offset overflow;
- virtual-address overflow;
- overlapping sections;
- malformed padding/reserved fields;
- missing signature coverage;
- modified metadata after signing;
- unsupported versions/algorithms;
- invalid authority identifiers.

### Crypto

- malformed public keys;
- undersized keys;
- malformed signatures;
- wrong algorithm identifiers;
- wrong authority type;
- signature over modified metadata;
- signature over modified payload;
- corrupted key identifiers.

### Files/tooling

- input/output path collision;
- missing executable;
- hostile tool output;
- tool non-zero exit status;
- interrupted tool execution;
- failed write followed by retry;
- temporary-file cleanup after failure.

---

## Security priority summary

| Priority | Area | Focus |
|---|---|---|
| Critical | Signing boundary | Authenticate all security-relevant CXEX data canonically |
| High | ELF | Complete structural validation and overflow-safe parsing |
| High | CXEX | Checked layout arithmetic and semantic validation |
| High | Trust model | Separate platform, publisher, and artifact authority |
| High | Crypto | Explicit algorithm/key/signature policy |
| High | Key handling | Protect private signing material |
| Medium/High | Filesystem | Safe path and atomic artifact handling |
| Medium/High | Tool execution | Explicit process and environment semantics |
| Medium | Reproducibility | Deterministic artifact generation |
| Medium/Long-term | Supply chain | Toolchain provenance and reproducible builds |

These priorities describe security importance and hardening order. They do not by themselves establish exploitability of any individual implementation.

---

## Conclusion

CX DevKit should be treated as part of the CX trust chain. Its job is not merely to convert compiler output into a different file format; it is to construct artifacts that will cross a security boundary into CXK.

The strongest architectural model is therefore defense in depth: DevKit validates aggressively, canonicalizes and signs only validated artifacts, and CXK independently validates every artifact again before execution.

The most important invariant is simple:

> **A valid DevKit signature must never substitute for CXK's own executable validation.**

If that invariant is preserved, a DevKit parser bug, malformed build input, or compromised build environment has a much smaller path to becoming a kernel compromise.
