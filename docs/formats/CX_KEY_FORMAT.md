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

The intended verification sequence is therefore:

```text
CXEX + CXSG
   |
   +-- parse signature metadata
   |
   +-- compute SHA-256 of signed CXEX range
   |
   +-- resolve fingerprint in trusted-key store
   |
   +-- verify RSA-2048 PKCS#1 v1.5/SHA-256 signature
   |
   +-- only then permit CXEX loading/execution
```

## Cryptographic Algorithms

Current implementation:

- RSA-2048
- RSA PKCS#1 v1.5 signatures
- SHA-256
- SHA-256 public-key fingerprints
- RSA public exponent as encoded by XKPK; the current generated key profile uses the conventional exponent 65537.

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
