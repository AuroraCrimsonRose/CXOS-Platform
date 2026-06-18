# CATX Extension Naming Formula

All CATX formats follow a structured naming convention designed to provide immediate identification of a file's domain and purpose.

## Formula

```text
X + Domain + Type
```

Where:

| Component | Description |
|------------|------------|
| X | CATX namespace prefix |
| Domain | Functional category |
| Type | Specific object type |

## Domain Codes

| Code | Domain |
|--------|--------|
| K | Kernel |
| C | Compiled |
| B | Boot |
| F | File Format |
| P | Package |
| R | Runtime |
| M | Media |
| N | Network |
| S | Security |
| A | Archive |
| T | Temporary |

## Type Codes

| Code | Type |
|--------|--------|
| EX | Executable |
| PK | Public Key |
| SK | Secret Key |
| SG | Signature |
| DL | Definition / Library |
| CF | Configuration |
| DT | Data |
| IN | Index |
| MF | Manifest |

Additional type identifiers may be introduced as required by the CATX ecosystem.

---

# Examples

## Kernel Executable

```text
XKEX
```

- X = CATX
- K = Kernel
- EX = Executable

Meaning:

```text
CATX Kernel Executable
```

Example:

```text
kernel.xkex
```

---

## Kernel Public Key

```text
XKPK
```

- X = CATX
- K = Kernel
- PK = Public Key

Meaning:

```text
CATX Kernel Public Key
```

Example:

```text
release.xkpk
```

---

## Kernel Secret Key

```text
XKSK
```

- X = CATX
- K = Kernel
- SK = Secret Key

Meaning:

```text
CATX Kernel Secret Key
```

Example:

```text
developer.xksk
```

---

# Design Goals

The CATX naming system is intended to:

- Provide human-readable format identification.
- Group related formats by functional domain.
- Allow tooling to infer broad file purpose without parsing contents.
- Maintain consistency across the CATX ecosystem.
- Support future expansion without breaking existing naming conventions.

A developer familiar with the naming formula should be able to make an educated guess about the purpose of an unfamiliar CATX format simply by reading its identifier.