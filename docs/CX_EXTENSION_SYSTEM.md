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

**The domain letter is why this is a pattern and not a set of mnemonics.** A
service descriptor could read `.xsvc` — shorter, and "svc" is more obviously
"service" than "sv" is. But it spells the *kind* of thing at the cost of saying
nothing about *whose* it is, and the kind is the part you can guess from
context. Whether a file belongs to the kernel, the boot chain, the operating
system or a user is the part you cannot, and it is the part that decides who
may write it and what is allowed to load it. `.xosv` keeps that: **x** · **o**
(OS) · **sv** (service). The same service concept in another domain would be
`.xcsv` or `.xksv`, and all three sort together and read the same way.

A name that fits the pattern is always preferable to a name that reads slightly
better on its own.

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
| `pk` | Public Key — public key material (verifies signatures) |
| `sk` | Secret/Signing Key — private key material (creates signatures) |
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
| `.xkpk` | Kernel Public Key (public; verifies signatures) |
| `.xksk` | Kernel Secret/Signing Key (private; signs artifacts; never shipped) |

### XB — CX Boot (Boot System)

| Extension | Meaning |
|-----------|---------|
| `.xbex` | Boot Executive (bootloader executable) |
| `.xbco` | Boot Configuration Object (boot parameters / config) |
| `.xbin` | Boot Binary Image (boot / disk / partition image) |

### XC — CX Compiled (build products)

| Extension | Meaning |
|-----------|---------|
| `.xcob` | Compiled Object (intermediate object file) |
| `.xcsl` | Compiled Static Library |
| `.xcdl` | Compiled Dynamic Library |
| `.xchi` | Compiled Header Interface (ABI / interface definition) |

A **category** domain, like XF and XA — these belong to no tier. They are what
a compiler emits or consumes without being runnable on their own, which is the
fact `c` is naming.

**`.xcex` is retired.** It named an executable for *how it was built*, and
every executable in this system is compiled — a kernel image is as compiled as
an application — so the letter distinguished nothing. An executable is named
for **whose** it is instead: `.xkex`, `.xbex`, `.xoex`, `.xsex`, `.xuex`. The
type code `0x4345 'CE'` is retired with it and is not reissued.

> If dynamic libraries ever become runtime-loadable and signed, *whose* a
> library is starts to matter, and they would want `.xsdl` / `.xudl` under the
> ownership tiers rather than staying here.

### XO — CX Operating System

| Extension | Meaning |
|-----------|---------|
| `.xoex` | OS Executive (the broker executive; CXEX `type_code` 0x4F45 `'OE'`) |
| `.xosv` | OS Service descriptor (what to run at boot, and with what authority) |

`.xosv` is **not** a CXEX container and has no `type_code` — it is a text file,
`key=value` lines, read by the service supervisor. It names a program; it is
not one. That is why it is XO rather than XC: the thing it describes is part of
the operating system's startup, even when the program it points at is an
ordinary `.xuex`.

```
exec=/Shared/Programs/hi.xuex    the program to run (required)
args=hello world                 passed as argv, after the program's own path
start=boot                       `boot` to start it; absent means leave it off
grants=console,disk              authority to hand it; default is console only
```

Descriptors live in `/System/Services/`. Authority defaults to the minimum on
purpose: a service that needs the disk has to say so in the file, where someone
reading the system can see what it was given.

### XS — CX System

| Extension | Meaning |
|-----------|---------|
| `.xsex` | System Executive (OS-owned, but not the OS itself; `type_code` 0x5345 `'SE'`) |

Things the operating system owns and ships without being the OS proper: the
shell, the service supervisor, system utilities. `caps_for()` starts one with
`GRANT_SYSTEM_BASELINE` — console and disk — which is narrower than the
executive's on purpose. A system program is OS-owned but is not the OS, so it
gets enough to say something and read its own files, and asks for anything more
by being started with it.

### XU — CX User

| Extension | Meaning |
|-----------|---------|
| `.xuex` | User Executive (a user's application; `type_code` 0x5545 `'UE'`) |

A user's own program. `caps_for()` gives it **nothing**: an application holds
no authority except what whatever started it chose to pass down. That is the
capability model's default, and it is the default precisely because it is the
one that is safe to get wrong.

### The ownership ladder

All five are the same CXEX container, distinguished only by `type_code`, and
`caps_for()` reads that code to decide what a program starts with when the
**kernel** launches it:

| | | `caps_for()` |
|---|---|---|
| `.xkex` | the kernel | ring 0 — not subject to this |
| `.xbex` | the boot chain | runs before any of it exists |
| `.xoex` | the OS proper, the broker | `GRANT_OS_BASELINE` |
| `.xsex` | OS-owned, not the OS | `GRANT_SYSTEM_BASELINE` (console + disk) |
| `.xuex` | a user's | nothing |

A signature says **who** an image is, never what it may do. The table above is
the only place identity becomes authority, and it applies to the kernel-launched
path alone. Anything started from ring 3 is **attenuated** against its launcher
instead — a subset of what that launcher already held, never more — so a
`.xsex` started by the executive gets what the executive chose to pass it, not
the baseline above.

### XF — CX Format (Data & Serialization)

| Extension | Meaning |
|-----------|---------|
| `.xfxn` | Format X Native (X Native source; compiled to an executive) |
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
| `XR` | Runtime (VM, JIT, execution metadata) |
| `XP` | Package (packages, installs, dependencies) |
| `XT` | Temporary (cache, staging, temp files) |

**`XS` is no longer reserved for Security.** It was listed here before the
domain restructure made `S` the **System** ownership tier (`.xsex`). A letter
cannot mean both. Keys already moved to where their authority lives — the
platform root is `.xkpk`, a publisher key `.xupk` — and certificates, auth and
policy files will need a letter of their own when they exist. That is an open
choice, not a reservation.

### Codes here are domains; language names are not

`XM`, `XN`, `XR` and the rest above are **domain codes**: `x` plus the letter
that appears directly after it in an extension. The X languages are never
abbreviated to two letters for exactly this reason — "XN" for X Native would
read as the Network domain, "XR" for X Runtime as the Runtime domain. Languages
are named in full, and their source lives in the **F** domain: `.xfxn`,
`.xfxr`, `.xfxh`, `.xfxd`, `.xfxv`, `.xfgl`. See `CX_ROADMAP.md` §2.

---

## 7. Design Intent

The system is designed to:

1. **Classify quickly at the filesystem level** — CXK can decide how to handle a
   file from its extension, without parsing contents.
2. **Separate intent from truth** — extension = expected behavior; header =
   permitted behavior.
3. **Support a multi-stage pipeline** — artifacts move through compilation
   (`.xcob`) → linking (`.xcsl` / `.xcdl`) → execution (`.xuex`).
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
| Signature verification (`.xkpk` / signatures) | a crypto/verification facility |
| Extension-based routing in the FS | CXFS support for richer metadata (v2+) |

The agreed build order — networking → Ring 3 → CXFS upgrade → kernel-as-a-file →
XFL modules → networked updates — is what makes this extension/type system
implementable. Until those land, this document is the design target, not a
shipped capability.

---

## 9. CXEX — The CXOS Executable Format

> **Status:** Finalized design for the *loadable executable* format used by
> `.xkex` (kernel) and `.xbex` (bootloader). This refines §5's proposed header
> into a concrete, sectioned, loadable layout. It does **not** apply to `.xbin`
> (a raw boot/disk/partition image with no loader header — an installer writes
> it byte-for-byte).

### 9.1 What CXEX is for

`.xkex` and `.xbex` are **executables**: the loader (the bootloader for the
kernel; whatever stage loads the bootloader for `.xbex`) must place them in
memory correctly and jump to their entry point. A flat blob can't express that —
code, read-only data, initialized data, and zero-filled `.bss` each need
distinct placement. CXEX is a **sectioned** format: a fixed header followed by a
section table describing where each piece loads.

Userspace executables (`.xuex`) reuse this same format; the only difference is
that they will typically be **relocatable** (see 9.5), whereas the kernel and
bootloader are **fixed-load**.

### 9.2 Layout

```
+----------------------+  offset 0
|  CXEX header         |  fixed size (§9.3)
+----------------------+
|  section table       |  section_count entries (§9.4)
+----------------------+
|  section data        |  the bytes for each non-BSS section,
|  ...                 |  at the file offsets named in the table
+----------------------+
|  relocation table    |  optional (reloc_offset; 0 = none)  (§9.5)
+----------------------+
|  signature block     |  optional (signature_offset; 0 = unsigned)
+----------------------+
```

### 9.3 Header

Extends §5. All integers little-endian.

| Field | Size | Purpose |
|-------|------|---------|
| `magic` | 4 | `"CXEX"` — identifies a CXOS executable |
| `type_code` | 2 | canonical type (mirrors extension: kernel-exec / boot-exec / …) |
| `format_version` | 2 | version of this header format |
| `arch_target` | 2 | target architecture (e.g. 1 = x86-32) |
| `abi_version` | 2 | ABI built against |
| `flags` | 4 | capability / load-policy bits (§9.6) |
| `entry_point` | 4 | virtual address to jump to once loaded |
| `load_base` | 4 | preferred base virtual address (fixed-load anchor) |
| `image_min` | 4 | lowest virtual address used by any section |
| `image_max` | 4 | highest virtual address used (incl. BSS) — total span to reserve |
| `section_count` | 2 | number of entries in the section table |
| `section_offset` | 2 | file offset of the section table |
| `reloc_offset` | 4 | file offset of the relocation table (0 = none / fixed) |
| `signature_offset` | 4 | file offset of the signature block (0 = unsigned) |
| `dependency_offset` | 4 | file offset of the dependency table (0 = none) |
| `reserved` | 8 | reserved for future fields (zeroed) |

`image_min`/`image_max` let the loader reserve the whole memory span in one step
before placing sections (important for `.bss`, which extends the span beyond the
stored bytes).

### 9.4 Section table

One entry per section (`.text`, `.rodata`, `.data`, `.bss`, …):

| Field | Size | Purpose |
|-------|------|---------|
| `name` | 8 | short section name (NUL-padded, e.g. `".text"`) |
| `file_offset` | 4 | where the section's bytes start in the file (0 if NOBITS) |
| `virt_addr` | 4 | virtual address to load the section at |
| `file_size` | 4 | bytes stored in the file |
| `mem_size` | 4 | bytes to occupy in memory |
| `flags` | 4 | R / W / X + NOBITS (see below) |

Section flags: `READ` (1), `WRITE` (2), `EXEC` (4), `NOBITS` (8).

- A normal section has `file_size == mem_size`: copy `file_size` bytes from
  `file_offset` to `virt_addr`.
- A **`.bss`** section is `NOBITS` with `file_size == 0` and `mem_size > 0`: the
  loader allocates `mem_size` bytes at `virt_addr` and **zero-fills** them —
  nothing is stored in the file (this is why zeros aren't wasted on disk).

### 9.5 Relocation (optional; off for kernel/boot)

`.xkex` and `.xbex` are **fixed-load**: linked at `load_base`, loaded there, no
relocation. `reloc_offset` is 0. (The kernel additionally gets virtual placement
"for free" from the MMU once paging is up — it can map itself to any virtual
address regardless of physical load location, so it never needs file-level
relocation.)

The format still *carries* relocation support so **userspace** executables
(`.xuex`), which benefit from being position-independent, can use it later:

- If `reloc_offset` != 0, it points at a table of relocation entries (each:
  `offset` to patch, `type`, optional `addend`), applied by the loader after
  sections are placed and before transferring control.
- The `RELOCATABLE` flag (§9.6) declares the image may be loaded somewhere other
  than `load_base`; the loader then adjusts addresses and applies the table.

Designing relocation in now (even though the kernel doesn't use it) means the
single format covers both fixed kernel/boot images and relocatable userspace
programs without a redesign.

### 9.6 Load-policy flags

The header `flags` field (extends §5.1):

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `EXECUTABLE` | the artifact may be executed |
| 1 | `RELOCATABLE` | may load away from `load_base` (apply reloc table) |
| 2 | `SIGNED` | a signature block is present and must verify |
| 3 | `KERNEL_PRIV` | requests ring-0 / kernel privilege |
| 4 | `REQUIRE_ABI_MATCH` | refuse to load on ABI mismatch |
| 5 | `REQUIRE_ARCH_MATCH` | refuse to load on arch mismatch |

The loader checks `magic`, `arch_target`, `abi_version`, and these flags
**before** placing or running anything — the header is the authority (§1).

### 9.7 Load procedure

To load a CXEX image:

1. Read and validate the header: `magic == "CXEX"`, `arch_target` matches,
   `abi_version` acceptable, required policy flags satisfied. Reject on mismatch.
2. Reserve the memory span `[image_min, image_max)` (or the relocated
   equivalent if `RELOCATABLE` and being placed elsewhere).
3. For each section in the table:
   - if `NOBITS`: zero-fill `mem_size` bytes at `virt_addr`;
   - else: copy `file_size` bytes from `file_offset` to `virt_addr` (and zero
     any `mem_size - file_size` tail).
4. If `reloc_offset` != 0 and relocating: apply the relocation table.
5. If `SIGNED`: verify the signature block before trusting the image.
6. Jump to `entry_point`.

For the **kernel** (`.xkex`) this is what the bootloader does (once it can read
the boot/kernel partition); for **`.xbex`** it is what the earliest stage does.
Until the filesystem-aware boot path exists, the kernel may still be loaded by
fixed offset (CXFS v2 §11.6.2 option b) with the CXEX header parsed to find
`entry_point` and the sections.

### 9.8 Relationship to ELF

`i686-elf-gcc` emits ELF, which already expresses sections, entry point, and
relocations. CXEX is intentionally a **simpler, CXOS-native** container: a flat
fixed header + a small section table that boot-time code can parse in a few
lines, plus the CXOS identity/policy/signature fields ELF lacks. The practical
build path is to **link as ELF, then convert** the loadable sections into a CXEX
image with a small build tool — keeping the toolchain standard while the on-disk
artifact is CXOS's own format.

---

## 10. Code Signing (`.xkpk` / `.xksk`)

> **Status:** Designed and **implemented**. `lib/crypto/{sha256,bignum,rsa}.c` and
> `lib/format/cxex_verify.c` implement the scheme below in full: the CXSG
> fingerprint identity check (`sha256` of the signer's `.xkpk` compared against the
> one embedded in the kernel) followed by RSA-verify of the signature over
> `[0, signature_offset)`. It is **enforced**, not merely available — `cxex_exec`
> (`cpu/exec.c`) refuses to run any image that does not return `CXEX_VERIFY_OK`,
> and the separate `caps_for(type, trusted)` policy layer means a valid signature
> establishes identity without itself granting authority.
>
> **Known gap:** `sys_spawn` takes an image already resident in the caller's memory
> and does **not** verify it. Kernel/disk-loaded executables are verified;
> ring-3-spawned ones are not. The intended fix is not to add verification to
> `spawn` but to make the verified path the only way new code enters the system —
> see `docs/CX_ABI.md` §7.10 (`exec_path`).
>
> This defines how CXOS artifacts are signed and verified. **Threat model, stated honestly:** this is
> **code safety / integrity / authenticity** — ensuring the kernel, drivers, and
> programs are the genuine, unmodified ones produced by the project. It is **not**
> tamper-proof secure boot: on a hobby OS booting from ordinary writable storage
> there is no hardware root of trust (no UEFI key store / TPM / fuses), so a
> sufficiently privileged on-disk attacker cannot be fully excluded. What this
> design *does* deliver — corruption detection and authenticity for everything
> the kernel loads after boot — is real and worth having.

### 10.1 Keys

| Extension | Role | Lives | Secrecy |
|-----------|------|-------|---------|
| `.xksk` | **Secret/signing key** — *creates* signatures | the build machine only | NEVER shipped |
| `.xkpk` | **Public key** — *verifies* signatures | `/System` (+ fingerprint embedded in kernel) | public |

(`pk` = public key, `sk` = secret key — the standard crypto keypair convention.)

- The **private key (`.xksk`) never leaves the compiling machine.** All signing
  happens offline at build time. The device only ever performs *verification*.
- The **public key (`.xkpk`) is shipped** and used by the kernel to verify the
  drivers, modules, and programs it loads.

### 10.2 Algorithm

**RSA-2048 signature over a SHA-256 hash** (RSA PKCS#1 v1.5).

1. Sign (offline, on the build machine): compute `SHA-256(artifact)`, then
   RSA-sign that digest with the private key. The result is the signature block.
2. Verify (on the device): recompute `SHA-256(artifact)`, RSA-verify the
   signature against it with the public key. Match = authentic and unmodified.

Why this construction:

- The **hash** provides integrity — any single changed bit changes the digest.
- The **RSA signature over the hash** provides authenticity — only the holder of
  the private key could produce a signature that verifies.
- **Verification is the cheap, simple half**: RSA verify is `sig^e mod n` with
  public exponent `e = 65537`, i.e. modular exponentiation on a 2048-bit bignum,
  plus a SHA-256 implementation. Both are very implementable from scratch and are
  the *only* crypto the kernel/bootloader need. Key generation and signing (the
  hard half) happen offline with standard tools (e.g. OpenSSL).
- **PKCS#1 v1.5** padding is specified rather than PSS: it is deterministic and
  far simpler to verify correctly, which matters for a from-scratch
  implementation (complexity is risk). SHA-256 is the hash, with room to upgrade
  the digest later via `format_version`.

### 10.3 Signature block

Referenced by `signature_offset` in the CXEX header (§9.3); present when the
`SIGNED` flag is set. The hash/signature cover the entire artifact **except** the
signature block itself (the block's own bytes are treated as zero while hashing,
so the signature can live inside the file it signs).

| Field | Size | Purpose |
|-------|------|---------|
| `sig_magic` | 4 | `"CXSG"` — marks a signature block |
| `sig_algo` | 2 | algorithm id (1 = RSA-2048 / SHA-256 / PKCS#1 v1.5) |
| `hash_algo` | 2 | digest id (1 = SHA-256) |
| `key_fingerprint` | 32 | SHA-256 of the public key this was signed against |
| `sig_len` | 2 | signature length in bytes (256 for RSA-2048) |
| `signature` | sig_len | the RSA signature over the digest |

`key_fingerprint` lets a verifier confirm it is using the *right* public key
before spending effort on the RSA math, and supports key rotation (multiple keys
distinguishable by fingerprint).

### 10.4 Trust anchor

The point that makes verification meaningful: **what checks the checker.**

- The kernel **embeds the SHA-256 fingerprint of the trusted public key** in its
  own binary (32 bytes, like the embedded copyright string). On startup it reads
  `/System/<key>.xkpk`, hashes it, and accepts it **only if the fingerprint
  matches the embedded one**.
- Consequence: although `.xkpk` sits on writable storage, swapping it for an
  attacker's key fails — the kernel will not trust a key whose fingerprint does
  not match the one baked into its already-running, already-trusted image. This
  anchors the **kernel-verifies-modules** chain in the kernel itself, which is a
  sound root of trust for everything loaded *after* boot.
- The weaker link is **boot-verifies-kernel**: the bootloader can verify the
  kernel's signature, but without a hardware anchor an attacker who can rewrite
  the kernel can usually also rewrite the bootloader and disable the check. So at
  the boot stage, signature verification is treated as **corruption/tamper
  *detection*** (very useful with in-place kernel updates — a bad or
  half-written kernel fails verification) rather than an unbreakable gate.

### 10.5 What gets signed

- `.xkex` (kernel), `.xbex` (bootloader) — verified at boot (integrity/detection).
- `.xkdr` (drivers), `.xklo` (kernel modules), eventually `.xuex` (userspace) —
  verified by the running kernel against the anchored `.xkpk` before load/run.
  This is where signing has its full strength.

### 10.6 Phased implementation

1. **Phase 1 — integrity (hash only).** Embed/verify a SHA-256 of each artifact.
   Catches corruption and accidental modification; implementable as soon as a
   SHA-256 routine exists. The signature block is used with `sig_algo` = hash-only.
2. **Phase 2 — authenticity (RSA-2048).** Add RSA-2048 verification + the
   embedded public-key fingerprint anchor. Upgrade in place — the block format and
   the `SIGNED` flag are designed to carry either, so no redesign is needed.

Signing depends on: a SHA-256 implementation (both phases), a bignum modexp for
RSA verify (phase 2), the loader (§9), and the `/System` protected area (CXFS v2)
to hold `.xkpk`.

---

*CX Design Spec — the CXOS file extension and type system. Forward-looking;
sections marked (proposed) are open for revision.*