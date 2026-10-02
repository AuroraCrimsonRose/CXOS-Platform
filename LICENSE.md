# CXOS licensing

CXOS is **source-available, not open source.** It is licensed in three tiers, and
which tier applies depends on **where a file lives in this repository**.

| Tier | Paths | Licence |
|---|---|---|
| **1. The operating system** | `boot/`, `kernel/`, `os/executive/`, `os/services/`, `os/apps/`, `os/config/` | [PolyForm Noncommercial 1.0.0](licenses/LICENSE-POLYFORM-NC-1.0.0.md) |
| **2. The SDK and tooling** | `abi/`, `os/std/`, `os/xc/`, `devkit/`, `editors/vscode/`, `tools/`, `docs/` | [MIT](licenses/LICENSE-MIT.md) |
| **3. Third-party components** | anything listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) | their own upstream licences |

Where a file carries an `SPDX-License-Identifier` header, that header governs.

**Licensor:** Aurora Tejeda, trading as CATX Systems.

---

## 1. The operating system — PolyForm Noncommercial 1.0.0

The kernel (CXK), the boot chain, the CXFS filesystem, the executive, the system
services and the system applications.

**You may** read, audit, build, modify, run and share these components **for any
noncommercial purpose** — personal use, study, research, teaching, evaluation and
hobby projects — under the terms of
[PolyForm Noncommercial 1.0.0](licenses/LICENSE-POLYFORM-NC-1.0.0.md), which is
the controlling text.

**You may not** use them for a commercial purpose without a separate commercial
licence. That includes shipping CXOS in or with a product, deploying it in a
business, embedding it in hardware, or offering it as a service.

**Commercial licensing:** aurora.tejeda@catxhosting.com

### Applications are independent works

This is the important part if you want to write software **for** CXOS, and it is
stated deliberately so nobody has to guess.

> A program written for CXOS is an **independent work**. Compiling against the
> headers in `abi/`, linking the X standard library in `os/std/`, invoking CXK
> system calls, using any documented user-mode API, and being packaged in the
> CXEX executable format **do not** make your program a derivative of the
> PolyForm-licensed components.
>
> **Your application is yours.** Licence it however you like, including a
> proprietary commercial licence, and sell it if you want to. Nothing in the
> PolyForm tier reaches it.

The same applies to drivers and extensions you write against the published
interfaces.

What the noncommercial restriction *does* reach is **running or distributing
CXOS itself** for a commercial purpose. A company may not deploy CXOS on its
machines, or ship a device with CXOS on it, on the strength of its application
being independently licensed. Those are separate questions and only the second
one is free.

## 2. The SDK and tooling — MIT

The ABI headers, the X standard library, the X compiler, the CX DevKit (the `cxk`
CLI and CX DevKit Studio), the VS Code extension, the build system and the
documentation are [MIT](licenses/LICENSE-MIT.md).

MIT deliberately, and for one reason: **everything an application developer has
to touch should impose nothing on them.** You can build, modify, redistribute and
commercially exploit these components, and you can vendor them into a proprietary
product. If a tool needed to write CXOS software were encumbered, the exemption
above would be worth nothing.

## 3. Third-party components

CXOS will come to include ported third-party software. Those components keep
their own upstream licences, which are **not** superseded by anything here.
Current inventory: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Trademarks and official releases

Neither tier grants any right to the names **CXOS**, **CXK**, **CX DevKit**, **X**
(as the language name) or **CATX Systems**, or to any associated logo, beyond
honest descriptive reference to this project.

Official CXOS releases are cryptographically signed. Only builds signed by CATX
Systems may be presented as official, and only CATX Systems operates the official
update channel. A fork may exist and be shared noncommercially; it may not call
itself CXOS or imply endorsement.

## Contributions

CXOS does not accept outside contributions at present. See
[CONTRIBUTING.md](CONTRIBUTING.md) before submitting anything — a contribution
made without a signed agreement in place cannot be merged, because the commercial
tier above requires unbroken relicensing authority over every line in it.

## No warranty

CXOS is an operating system. It runs in kernel mode, writes to storage devices
and controls hardware directly. It is provided **as is**, without warranty of any
kind, and you assume all risk of data loss, system failure and hardware damage.
Full disclaimers are in the licence texts of each tier.

Security reports: [SECURITY.md](SECURITY.md).
