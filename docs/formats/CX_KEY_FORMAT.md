# CX Key and Signature Format

## Purpose

This document defines the key, trust, and artifact-signature formats used by CXOS/CXK for authenticating CXEX executable images.

The security model is:

```text
signing key pair
    |
    +-- .xksk  private signing key
    |
    +-- .xkpk  serialized public key
                    |
                    v
              CXSG signature block
                    |
                    v
bootloader -> CXK -> verify CXEX -> load/execute
                    ^
                    |
             trusted key store
```

CXK performs verification before CXEX code is loaded or executed. The public key embedded in an artifact identifies the signer, but the embedded key is not trusted merely because it is present. CXK's trusted-key store/root of trust determines whether that signer is authorized.

## Key Types

### XKPK — CX public key

The `.xkpk` format is the serialized public key consumed by the kernel and tooling.

Current RSA layout:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic: `CXPK` |
| 4 | 2 | Format version |
| 6 | 2 | Key size in bits |
| 8 | 4 | RSA public exponent |
| 12 | 2 | Modulus length in bytes |
| 14 | 2 | Reserved |
| 16 | N | RSA modulus, big-endian |

The current CXK RSA verifier is implemented for RSA-2048, using a 256-byte modulus.

The modulus is stored in big-endian order. The serialized `.xkpk` bytes are significant to key identity: CXK's trust-store fingerprinting hashes the exact serialized public-key bytes with SHA-256.

### XKSK — CX private signing key

The `.xksk` file represents the private signing authority used by host-side tooling.

The current tooling stores the private key as PEM and imports it through the platform .NET cryptography APIs. The private key is never embedded into CXEX artifacts.

Private `.xksk` material must remain outside source control and must be protected as signing authority. Possession of a trusted private key permits creation of artifacts that CXK will recognize as authentic.

The public `.xkpk` file is not secret.

## CXSG — CXEX Signature Block

A signed CXEX image ends with a CXSG signature block.

Current layout:

| Field | Size | Description |
|---|---:|---|
| Magic | 4 | `CXSG` |
| Signature algorithm | 2 | Current value: RSA-2048/SHA-256 |
| Hash algorithm | 2 | Current value: SHA-256 |
| Public-key fingerprint | 32 | SHA-256 of the exact serialized `.xkpk` bytes |
| Public-key length | 2 | Length of embedded `.xkpk` |
| Signature length | 2 | RSA signature length |
| Public key | N | Embedded serialized `.xkpk` bytes |
| Signature | N | RSA PKCS#1 v1.5 signature |

The embedded public key allows the verifier to identify which serialized public key was used. Trust is still determined by the CXK trusted-key store.

For the current RSA-2048 implementation, the signature is 256 bytes.

## Signed Byte Range

The signer first loads and validates the CXEX image, then:

1. Refuses to sign an image that already contains a signature.
2. Sets the CXEX signed flag.
3. Writes the signature offset to the end of the unsigned CXEX image.
4. Computes SHA-256 over the complete CXEX byte range up to, but not including, the CXSG block.
5. Signs that digest with RSA-2048 PKCS#1 v1.5 using SHA-256.
6. Appends the CXSG block.

The signature therefore authenticates the exact serialized CXEX image that CXK will load.

The signature does not cover the CXSG block itself. The public-key fingerprint and signature metadata are instead parsed separately by the verifier.

## Trust Model

There are three distinct concepts:

### 1. Identity

The CXSG public-key fingerprint answers:

> Which serialized public key does this artifact claim to have been signed with?

### 2. Cryptographic validity

The RSA signature answers:

> Does the signature mathematically authenticate the CXEX bytes using that public key?

### 3. Authorization

The CXK trusted-key store answers:

> Is this public key one that this system is willing to trust?

A valid signature from an untrusted key must not be sufficient to execute an artifact.

The verification sequence is **integrity first, identity second**, and the
order is load-bearing rather than stylistic:

```text
CXEX + CXSG
   |
   +-- parse signature metadata
   |
   +-- compute SHA-256 of the signed CXEX range
   |
   +-- verify RSA-2048 PKCS#1 v1.5/SHA-256 against the key the image CARRIES
   |      (nothing below is worth asking until the bytes are known to be
   |       what the signer signed)
   |
   +-- resolve that key's identity: platform, publisher, or neither
   |
   +-- only then permit CXEX loading/execution, per the caller's policy
```

Earlier revisions of this document put the trusted-key lookup *before* the
signature check. The implementation has always done it the other way round
(`keyvault_trust_of`, `kernel/cpu/keyvault.c`), and the implementation is
right: resolving a fingerprint first means making a trust decision about
metadata that has not yet been authenticated, and the fingerprint is part of
the signed range.

### Authority levels

Three levels, ordered so that a policy reads as a minimum
(`enum cx_trust`, `kernel/cpu/keyvault.h`; `CXTrustLevel` in
`devkit/CXEX.Crypto/Trust/CXAuthChain.cs` mirrors it):

| Level | Means |
|---|---|
| `PLATFORM` (2) | The signing key is **byte-for-byte** the key compiled into this kernel. |
| `PUBLISHER` (1) | The signing key is in `/System/KeyVault` — someone decided to believe this publisher. |
| `UNVERIFIED` (0) | Valid signature, by a key this machine has never been told to believe. A real answer, and not the same as tampered. |

**The authority invariant:** a level is *derived from which key signed*, and
from nothing the artifact says about itself. There is deliberately **no**
authority, tier or privilege field in CXEX for a signer to fill in, so there is
nothing for a publisher to claim and nothing for a verifier to have to
disbelieve. What an image may then do follows from its level
(`kernel/cpu/exec.c`), never from a flag it carries — `CXEX_FLAG_KERNEL_PRIV`
exists in the format and is deliberately not consulted by any trust decision.

**Importing a key can make a publisher; it can never make the platform.** The
platform key is compared byte for byte rather than looked up, because it is
compiled into the kernel rather than stored in the vault — so it cannot be
added, removed or replaced by anything with write access to a disk. The DevKit
takes the same route for the same reason: a key store is something a developer,
a build script or an installer can add to.

### Secure Boot is a separate trust domain

A key trusted to authenticate firmware is **not** a key authorized to sign user
executables, and the separation is structural rather than a rule to remember:
Secure Boot uses X.509 (`cxk secureboot keygen` writes `.pem`/`.cer`) while CXEX
uses CXPK, and the CXPK parser enforces one profile — so Secure Boot material
cannot be loaded into a CXEX key store at all, let alone trusted by it. The two
chains can meet in one overall boot story without either becoming the other.

## Cryptographic Algorithms

**One profile, and it is enforced rather than conventional.** Every parameter
below is checked on both sides; a key or signature outside the profile is
refused, not accepted-if-parseable (DevKit security review §6).

| Parameter | Value | Refused if otherwise |
|---|---|---|
| Signature algorithm | RSA PKCS#1 v1.5 | yes |
| Hash | SHA-256 | yes |
| Key size | RSA-2048, modulus exactly 256 bytes | yes |
| Public exponent | **65537, pinned** | yes |
| CXPK version | 1 | yes |
| CXPK `reserved` | 0 | yes |
| Fingerprint | SHA-256 over the serialized `.xkpk` bytes | — |
| Signature length | must equal the modulus length | yes |

The exponent is the one that matters most, and it is pinned rather than
"conventional": `e = 1` makes RSA verification the identity function and every
signature forgeable, and an even `e` is not an exponent at all. Both parsed
cleanly before 2026-10-02.

The constants live in two places on purpose — `RSA_PROFILE_*` in
`kernel/lib/crypto/rsa.h` and `CXKeyGenerator.PROFILE_*` in the DevKit —
declared separately rather than shared so that CXK does not depend on the
DevKit to decide what CXK can verify. A test compares them
(`CryptoPolicyTests`), because separately declared means they can drift.

Policy is enforced in the **crypto layer**, not in the CLI. `cxk keygen`
refusing `--bits 4096` is a convenience; `CXKeyGenerator.Generate` refusing it
is the guarantee, and before 2026-10-09 only the former existed — so Studio or
any other caller of the library could mint a key that signs perfectly and whose
every artifact is refused at boot as `BAD_SIGNATURE`, which reads as tampering
rather than as the wrong key size.

**No algorithm agility.** There is no negotiation and no "any algorithm the
framework can parse". A new algorithm is added by changing the profile on both
sides in one commit, bumping the XKPK format version, and extending the policy
tests — never by accepting an identifier that happens to be recognised.

The host-side signer uses the platform .NET RSA implementation. CXK contains its own verification-only RSA, bignum, and SHA-256 implementations because the kernel must verify artifacts before relying on higher-level OS facilities.

No private-key operation is performed by CXK.

## Key Pair Consistency

The host-side signer verifies that the RSA modulus exported from the private key matches the modulus in the supplied `.xkpk` file.

The signer also requires the resulting signature length to match the public-key modulus length.

This prevents accidentally producing an artifact whose embedded public key does not correspond to the private key used to create the signature.

## Security Requirements

### Private keys

- Never commit `.xksk` files containing private signing material.
- Never embed private key material in CXEX, CXOS, CXK, or generated source.
- Restrict access to signing keys.
- Treat a compromised trusted private key as a trust-root compromise.
- Rotate/revoke a key through the trusted-key mechanism rather than silently replacing the serialized key.

**Handling, as audited 2026-10-09 (DevKit security review §7).** What the
tooling actually does, rather than what it ought to:

- **Permissions.** `cxk keygen` creates the `.xksk` **owner-only**, and at
  creation rather than by chmod afterwards, so the file never exists with any
  other mode. Before this it was written with `File.WriteAllText`, i.e. the
  default mode, which umask normally leaves world-readable — a private signing
  key readable by every user on the machine for the whole life of the file. On
  Windows the inherited directory ACL governs instead; `UnixCreateMode` cannot
  be set there at all (its setter throws), which is itself worth knowing.
- **Write order.** The public half is written first. If the second write
  fails, what is left on disk is a public key with no private half — useless to
  everyone — rather than a private signing key with nothing to pair it with,
  in a directory the caller may not know it needs to clean.
- **Plaintext persistence is accepted, not solved.** The private key is an
  unencrypted PKCS#8 PEM, because an unattended `cxk os build --key test` has
  to be able to sign. The mitigation is permissions, `.gitignore` coverage and
  the key never being an argument; a protected store or handle-based signing
  API is the real answer and is not built. Recorded so it is a known position
  rather than an oversight.
- **Not on the command line.** Keys are passed by *path* (`--key <name>`
  resolving `tools/<name>.xksk`), never as material, so key bytes never reach
  a process argument list, a shell history or a CI log.
- **Not in output.** `cxk` prints and logs paths and fingerprints, never key
  bytes, and no exception message carries private material — the signer's
  mismatch error names the modulus length, not the modulus.
- **Not in source control.** `*.xksk`, `*.xusk`, `*.pem`, `*.pfx`, `*.cer`,
  `sbkeys/` and `tools/*.xkpk` (except the tracked platform public half) are
  gitignored, and neither merged history ever contained any — checked before
  the merge.
- **Not in build output.** `trusted_key.c` is generated into `build/` from the
  **public** half only, and `dist/` receives signed artifacts, never keys.

### Public keys

Public `.xkpk` files may be distributed with artifacts and tooling.

An embedded public key is not a secret and does not provide signing authority.

### Exact-byte identity

Because the trusted-key fingerprint is calculated over the serialized `.xkpk` bytes, changes to the serialization affect key identity even if the underlying mathematical RSA key is equivalent.

Implementations should therefore preserve the canonical XKPK serialization.

## Current Format Constraints

The current CXK implementation is specifically an RSA-2048 verifier:

- modulus length: 256 bytes
- modulus size: 2048 bits
- signature length: 256 bytes
- hash: SHA-256
- signature padding: PKCS#1 v1.5

The format currently contains version and key-size fields, but the kernel parser should enforce the values supported by the active algorithm definition rather than treating those fields as informational.

The host-side key generator currently permits RSA key sizes beyond 2048 bits. That is broader than the current CXK verifier contract. Until multi-size RSA support is explicitly added to the kernel and format specification, production key generation should be constrained to RSA-2048.

## Compatibility and Future Algorithms

The CXSG algorithm identifier exists so the format can distinguish cryptographic profiles in the future.

A future algorithm must define, at minimum:

- algorithm identifier
- key serialization format
- key-size/parameter rules
- signature encoding
- hash function
- signed byte range
- trust-store identity rules
- kernel verification requirements

Changing the cryptographic algorithm must not silently reinterpret an existing algorithm identifier.

## Implementation References

Host-side implementation:

- `devkit/CXEX.Crypto/Signing/CXSigner.cs`
- `devkit/CXEX.Crypto/Signing/CXVerifier.cs`
- `devkit/CXEX.Crypto/Trust/CXAuthChain.cs`
- `devkit/CXEX.Crypto/Trust/CXKeyGenerator.cs`
- `devkit/CXEX.Crypto/Trust/CXKeyStore.cs`

Kernel implementation:

- `kernel/lib/crypto/rsa.c`
- `kernel/lib/crypto/rsa.h`
- `kernel/lib/crypto/bignum.c`
- `kernel/lib/crypto/bignum.h`
- `kernel/lib/crypto/sha256.c`
- `kernel/lib/crypto/sha256.h`

## Status

This document describes the current CXOS/CXK signing design as implemented on the development branch.

The following items remain explicit hardening/compatibility work:

1. Enforce the RSA-2048 contract consistently in key generation and XKPK parsing.
2. Validate XKPK version, key-size, reserved fields, and the intended exponent policy in the kernel parser.
3. Keep the trusted-key root separate from the public key embedded in an artifact.
4. Preserve exact-byte signature coverage and reject malformed or ambiguous signature boundaries.
5. Add any future cryptographic algorithm only with a distinct, versioned algorithm identifier.
