# CXOS licensing

CXOS is **source-available, not open source.** It is licensed in two tiers, plus
third-party components under their own terms.

## How to tell which licence applies

**The `SPDX-License-Identifier` header in a file is controlling.** Open the file
and read its first line; that is the licence, and it governs regardless of
anything else written here.

```
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
```

Both identifiers used here are registered on the
[SPDX License List](https://spdx.org/licenses/), so standard tooling resolves
them without any custom mapping.

| SPDX identifier | Licence text | Covers |
|---|---|---|
| `PolyForm-Noncommercial-1.0.0` | [LICENSES/PolyForm-Noncommercial-1.0.0.txt](LICENSES/PolyForm-Noncommercial-1.0.0.txt) | the operating system |
| `MIT` | [LICENSES/MIT.txt](LICENSES/MIT.txt) | the SDK and tooling |

The paths below are a **structural fallback only**, for files that carry no SPDX
header: data files, assets, configuration, documentation. Where a header and
this table ever disagree, the header wins.

| Fallback paths | Licence |
|---|---|
| `boot/`, `kernel/`, `os/executive/`, `os/services/`, `os/apps/`, `os/config/` | `PolyForm-Noncommercial-1.0.0` |
| `abi/`, `os/std/`, `os/xc/`, `devkit/`, `editors/vscode/`, `tools/`, `docs/` | `MIT` |
| listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) | upstream licence |

**Licensor:** Aurora Tejeda, trading as CATX Systems.

### Required notice

PolyForm's *Notices* section obliges anyone who passes on any part of the
PolyForm-licensed components to pass on the licence (or its URL) **and any
plain-text line beginning with `Required Notice:` that the licensor supplied**.
This is that line:

```
Required Notice: Copyright (c) 2026 Aurora Tejeda (trading as CATX Systems) https://github.com/AuroraCrimsonRose/CXOS-Platform
```

Keep it with the software when you redistribute it. It is supplied here rather
than written into `LICENSES/PolyForm-Noncommercial-1.0.0.txt`, because that file
is the licence **verbatim**. Editing the worked example inside it would make it
a bespoke licence that no longer matches its SPDX identifier.

---

## The operating system: PolyForm Noncommercial 1.0.0

The kernel (CXK), the boot chain, the CXFS filesystem, the executive, the system
services and the system applications.

**You may** read, audit, build, modify, run and share these components **for any
noncommercial purpose**, under the terms of
[PolyForm Noncommercial 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt), which
is the controlling text. The licence states its own scope, and it is broader than
"hobbyists only": personal study, research, experiment, teaching, amateur
pursuits. It also covers **charitable organisations, educational institutions,
public research bodies, public safety and health organisations, environmental
organisations and government institutions**, regardless of how they are funded.

**You may not** use them for a commercial purpose.

### Commercial licensing is not available yet

> **There is currently no commercial licence to buy, and no commercial use is
> permitted.** Commercial licensing is **paused pending formation of a legal
> entity**. CATX Systems is presently a sole proprietorship, and a commercial
> agreement should be issued by a company, not a person.
>
> This is a deliberate closure, not an oversight. Until it lifts, commercial use
> of the PolyForm-licensed components is simply not licensed, and no exception
> can be granted informally.
>
> Enquiries, so you can be told when it opens: open a
> [discussion](https://github.com/AuroraCrimsonRose/CXOS-Platform/discussions) or
> an [issue](https://github.com/AuroraCrimsonRose/CXOS-Platform/issues) asking
> about commercial licensing. There is deliberately no contact address here: a
> published address is a permanent target, and nothing about this needs one until
> there is something to sell.

### Applications are independent works

This is the part that matters if you want to write software **for** CXOS, and it
is stated outright so nobody has to guess.

> A program written for CXOS is an **independent work**. Compiling against the
> headers in `abi/`, linking the X standard library in `os/std/`, invoking CXK
> system calls, using any documented user-mode API, and being packaged in the
> CXEX executable format **do not** make your program a derivative of the
> PolyForm-licensed components.
>
> **Your application is yours.** Licence it however you like, including a
> proprietary commercial licence, and sell it if you want to. Nothing in the
> PolyForm tier reaches it.

The same applies to drivers and extensions written against the published
interfaces.

What the noncommercial restriction *does* reach is **running or distributing
CXOS itself** commercially. A company may not deploy CXOS on its machines, or
ship a device with CXOS on it, on the strength of its application being
independently licensed. Those are separate questions, and today only the second
one is free.

## The SDK and tooling: MIT

The ABI headers, the X standard library, the X compiler, the CX DevKit (the `cxk`
CLI and CX DevKit Studio), the VS Code extension, the build system and the
documentation are [MIT](LICENSES/MIT.txt).

MIT deliberately, for one reason: **everything an application developer must
touch should impose nothing on them.** Build, modify, redistribute and
commercially exploit these components, including vendoring them into a
proprietary product. If a tool needed to write CXOS software were encumbered, the
exemption above would be worth nothing.

## Third-party components

Ported third-party software keeps its own upstream licence, which is **not**
superseded by anything here. Inventory:
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Trademarks and official releases

Neither tier grants any right to the names **CXOS**, **CXK**, **CX DevKit**, **X**
(as the language name) or **CATX Systems**, or to any associated logo, beyond
honest descriptive reference to this project.

Official CXOS releases are cryptographically signed. Only builds signed by CATX
Systems may be presented as official, and only CATX Systems operates the official
update channel. A fork may exist and be shared noncommercially; it may not call
itself CXOS or imply endorsement.

## Contributions

**CXOS is not accepting outside contributions**, and will not until a legal
entity exists to receive them. See [CONTRIBUTING.md](CONTRIBUTING.md).

## No warranty

CXOS is an operating system. It runs in kernel mode, writes to storage devices
and controls hardware directly. It is provided **as is**, without warranty of any
kind, and you assume all risk of data loss, system failure and hardware damage.
The full disclaimers are in each tier's licence text.

Security reports: [SECURITY.md](SECURITY.md).
