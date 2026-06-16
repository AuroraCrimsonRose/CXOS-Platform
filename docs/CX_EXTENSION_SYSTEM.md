# CXOS File Extension & Type System
### CX Design Spec — Aurora Tejeda / CATX SYSTEMS LLC

> **Status: DESIGN / PROPOSAL.** This describes the *intended* file-type system
> for CXK and the broader CXOS ecosystem. It is a forward-looking design, not a
> shipped feature — CXK does not yet classify, route, or load files by these
> rules. This document is the reference the eventual implementation will follow.
> Sections marked **(proposed)** are open design suggestions, not yet decided.

---

## 1. Overview

This spec defines the CXOS file-type system: a set of file extensions plus an
in-file header format that together let CXK classify and (eventually) execute
artifacts safely.

The core design principle is a **two-layer model**:

> **File extension = intent / routing hint**
> **File header = authority / validation / enforcement**

- The **extension** describes what a file is *expected* to do. It lets CXK route
  and classify an artifact quickly, without reading its contents.
- The **header** (inside the file) defines the *truth*: what the file actually
  is, and whether it is valid, compatible, and permitted to run.

This produces a two-stage pipeline:

1. **Fast-path classification** — by extension, no parsing required.
2. **Verified execution policy** — by header, before anything is trusted or run.

The extension is a hint; the header is the authority. If they disagree, the
header wins (and the mismatch is itself a signal something is wrong).

---

## 2. Naming Rule

Every CXOS extension follows a fixed pattern:

```
x + <subsystem> + <object-type>
```

- **`x`** — the CX ecosystem identifier (every CXOS type starts with `x`).
- **`<subsystem>`** — a one-letter family code (`k` = kernel, `b` = boot,
  `c` = compiled/userspace, `f` = format, `a` = archive, …).
- **`<object-type>`** — a two-letter code describing the kind of object.

So `.xkdr` parses as **x**(CX) · **k**(kernel) · **dr**(driver) = "CX kernel
driver."

### 2.1 Object-Type Vocabulary

The two-letter object-type suffix is meant to be **reused consistently** across
subsystems, so the system stays learnable as it grows. Common suffixes:

| Suffix | Meaning |
|--------|---------|
| `ex` | Executive — a primary executable for that subsystem |
| `co` | Configuration Object — config / state |
| `lo` | Library Object — a loadable module / library |
| `dr` | Driver Object — a driver module |
| `ob` | Object — an intermediate compiled object |
| `sl` | Static Library |
| `dl` | Dynamic Library |
| `hi` | Header Interface — an ABI / interface definition |
| `sk` | Signed Key — public signing key material |
| `pk` | Private Key — private/secret key material |
| `in` | Binary Image — a raw image (boot / disk / partition) |
| `to` | Text Object |
| `sl` | Scripting Language (in the format family) |
| `on` | Object Notation |
| `sf` | Storage Format |
| `cf` | Compression Format |

> When defining a new subsystem (Section 6), prefer an existing suffix from this
> table over inventing a new one, so `ex`/`co`/`lo` mean the same thing
> everywhere.

---

## 3. Defined Extensions

### XK — CX Kernel (Core System)

| Extension | Meaning |
|-----------|---------|
| `.xkex` | Kernel Executive (kernel binary / core executable) |
| `.xkco` | Kernel Configuration Object (kernel config / state) |
| `.xklo` | Kernel Library Object (internal kernel module / library) |
| `.xkdr` | Kernel Driver Object (kernel driver module) |
| `.xksk` | Kernel Signed Key (public signing key material) |
| `.xkpk` | Kernel Private Key (secure / private key material) |

### XB — CX Boot (Boot System)

| Extension | Meaning |
|-----------|---------|
| `.xbex` | Boot Executive (bootloader executable) |
| `.xbco` | Boot Configuration Object (boot parameters / config) |
| `.xbin` | Boot Binary Image (boot / disk / partition image) |

### XC — CX Compiled (Userspace)

| Extension | Meaning |
|-----------|---------|
| `.xcex` | Compiled Executive (userspace executable / application) |
| `.xcob` | Compiled Object (intermediate object file) |
| `.xcsl` | Compiled Static Library |
| `.xcdl` | Compiled Dynamic Library |
| `.xchi` | Compiled Header Interface (ABI / interface definition) |

### XF — CX Format (Data & Serialization)

| Extension | Meaning |
|-----------|---------|
| `.xfto` | Format Text Object |
| `.xfsl` | Format Scripting Language |
| `.xfon` | Format Object Notation |
| `.xflo` | Format Logging Object |

### XA — CX Archive (Containers & Packaging)

| Extension | Meaning |
|-----------|---------|
| `.xasf` | Archive Storage Format |
| `.xacf` | Archive Compression Format |

---

## 4. Partial Artifacts

Partial artifacts represent files that are incomplete — mid-download, mid-write,
or staged for an update — and must **not** be treated as valid by the loader
until finalized.

### 4.1 `.xprt` — Typed Partial (suffix marker)

`.xprt` is appended to an *existing* typed extension to mark that specific typed
artifact as partial. The underlying type is still visible.

```
config.xkco.xprt      a kernel config, still being written
library.xcob.xprt     a compiled object, not yet complete
archive.xasf.xprt     an archive mid-transfer
```

**Rule:** use `.xprt` when you know the final type and want to preserve it
through the partial stage. Finalizing = removing the `.xprt` suffix.

### 4.2 `.xpart` — Standalone Partial

`.xpart` is a self-contained partial artifact whose final type is not yet
expressed in the name (e.g. a generic update blob).

```
update.xpart
firmware.xpart
```

**Rule:** use `.xpart` when the artifact is partial and its final typed name
isn't applied yet. The final type is determined from its header on finalization.

> In both cases the **header** is the authority on whether the artifact is
> actually complete and valid — the partial extension is only a routing hint
> that says "do not load yet."

---

## 5. The Header (proposed)

The extension is a hint; the **header** is where authority lives. Every
executable or loadable CXOS artifact should begin with a header that CXK reads
*before* trusting or running it. The extension and the header's declared type
must agree.

> **(Proposed)** The following is a suggested header layout for discussion — not
> finalized. The intent is to capture identity, compatibility, capability, and
> integrity in a fixed, fast-to-read prefix.

| Field | Size | Purpose |
|-------|------|---------|
| `magic` | 4 | identifies a CXOS artifact (e.g. `"CXOS"`) |
| `type_code` | 2 | the canonical type (mirrors the extension; header is authoritative) |
| `format_version` | 2 | version of this header format |
| `arch_target` | 2 | target architecture (e.g. x86-32) |
| `abi_version` | 2 | ABI the artifact was built against |
| `flags` | 4 | capability / execution-policy bits (see below) |
| `body_offset` | 4 | byte offset where the payload begins |
| `body_length` | 4 | payload length in bytes |
| `dependency_offset` | 4 | offset to the dependency table (0 = none) |
| `signature_offset` | 4 | offset to the signature block (0 = unsigned) |
| `reserved` | … | reserved for future fields |

### 5.1 Capability / policy flags (proposed)

The `flags` field (and/or a richer policy section) would eventually express:

- **ABI compatibility** — refuse to load against an incompatible ABI.
- **Architecture targeting** — refuse to load a wrong-arch artifact.
- **Security capabilities** — what the artifact is permitted to do.
- **Execution permissions** — whether it may run at all, and at what privilege.
- **Dependency graph** — what else must be present/loaded first.

These enforcement layers are **future work** — they depend on CXK having a
process model, a loader, and (for signatures) a crypto/verification facility.

---

## 6. Reserved Future Namespaces

Subsystem letters reserved for later use. They will reuse the Section 2.1
object-type vocabulary.

| Code | Family |
|------|--------|
| `XM` | Media (audio, video, images, fonts) |
| `XN` | Network (protocols, packets, configs) |
| `XS` | Security (keys, certs, auth, policies) |
| `XR` | Runtime (VM, JIT, execution metadata) |
| `XP` | Package (packages, installs, dependencies) |
| `XT` | Temporary (cache, staging, temp files) |

---

## 7. Design Intent

The system is designed to:

1. **Classify quickly at the filesystem level** — CXK can decide how to handle a
   file from its extension, without parsing contents.
2. **Separate intent from truth** — extension = expected behavior; header =
   permitted behavior.
3. **Support a multi-stage pipeline** — artifacts move through compilation
   (`.xcob`) → linking (`.xcsl` / `.xcdl`) → execution (`.xcex`).
4. **Enable future enforcement** — headers grow to define ABI compatibility,
   architecture targeting, security capabilities, dependency graphs, and
   execution permissions.

### Key Architectural Insight

CXOS is not just defining file types — it is defining a **typed execution
ecosystem** where the filesystem participates in execution decisions, the kernel
uses extensions as routing metadata, and the header is the authority boundary.
This lets CXK evolve into a deterministic, policy-driven loader without
sacrificing performance or clarity.

---

## 8. Relationship to the Current CXK Roadmap

This system is aspirational and sits on top of foundations that do not exist
yet. Honest dependency ordering:

| This spec needs… | …which depends on |
|------------------|-------------------|
| A loader that reads headers | Ring 3 + process model |
| Loadable libraries (`.xcdl`, `.xklo`) | the **XFL** module system |
| Signature verification (`.xksk` / signatures) | a crypto/verification facility |
| Extension-based routing in the FS | CXFS support for richer metadata (v2+) |

The agreed build order — networking → Ring 3 → CXFS upgrade → kernel-as-a-file →
XFL modules → networked updates — is what makes this extension/type system
implementable. Until those land, this document is the design target, not a
shipped capability.

---

*CX Design Spec — the CXOS file extension and type system. Forward-looking;
sections marked (proposed) are open for revision.*