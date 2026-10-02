# Security policy

CXOS is an operating system, so most of what it does is a security boundary of
some kind. This document says **which boundaries are real**, which are
deliberately absent, and how to report something that crosses one.

It is written to be useful rather than reassuring. Where a protection does not
exist yet, it says so by name.

## Reporting a vulnerability

**This repository is private.** GitHub's private vulnerability reporting is a
public-repository feature and cannot be enabled here — the API returns 404 — so
there is no in-product reporting channel at present. Anyone who can read this
file can already reach the maintainer directly; use whatever private channel you
already have, and do not open an issue.

> **Maintainers: fill in a contact address below, and revisit this section the day
> the repository becomes public.** This file deliberately does **not** publish a
> personal email address; choose one you are willing to have indexed, such as a
> role address.
>
> **Security contact:** _not yet set_
>
> On going public, turn on **Settings → Advanced Security → Private vulnerability
> reporting**, and make it the primary channel in this section. It creates a draft
> advisory visible only to the maintainer, so nothing is disclosed while it is
> being fixed.

Please include the CXOS version (`cxk --version`, or the kernel banner at boot),
whether the kernel was a **development** build, and the smallest input or steps
that reproduce it. A CXEX image, disk image or X source that triggers the problem
is worth more than a description of it.

**What to expect.** CXOS is maintained by one person. There is no on-call
rotation and no guaranteed response time. Reports are read; a fix for a real
boundary crossing takes priority over anything else in the plan.

Please do not open a public issue for a vulnerability, and please do not test
against infrastructure you do not own. There is no bug bounty.

## Supported versions

| Version | Supported |
|---|---|
| 1.0.x | ✅ |
| < 1.0 | ❌ — pre-release, never published |

There is one supported line at a time. CXOS makes **no backwards-compatibility
promise**: formats, the ABI and interfaces change freely between releases, and
every user of a changed interface is updated in the same commit. A fix is
delivered as a new release, not as a patch to an old one.

## What is a security boundary

These are the claims CXOS actually makes. A way to break one is a vulnerability.

### 1. Ring 3 cannot reach the kernel

A loaded image runs in ring 3 and must not be able to read, write or execute
kernel memory, or to make the kernel do so on its behalf.

- Every section of an image must lie wholly below `KERNEL_VBASE`, checked in
  64-bit arithmetic so the sum cannot wrap into range.
- `paging_map_user` refuses any address at or above `KERNEL_VBASE`;
  `paging_map_kernel` refuses `PAGE_USER`. Neither entry point can express a
  ring-3-reachable kernel mapping.
- Every syscall pointer is checked with `user_ptr_readable` or
  `user_ptr_writable` for the access it actually performs. Ring 0 ignores the
  read-only page bit unless CR0.WP is set, so "present and user" is not enough to
  justify a write.

### 2. Only verified code runs

On a **release** kernel, an image is executed only if it carries a valid CXSG
signature over its bytes, by the key compiled into the kernel as its root of
trust.

- Everything the signature covers must end at `signature_offset`; a section whose
  bytes reach past it is refused, because that data is not signed.
- The kernel validates independently of the DevKit. Both sides check; neither
  trusts the other's check.

### 3. An image cannot exhaust the machine

- The loader validates an image whole before allocating a single frame, so a
  refusal costs nothing.
- Image, stack and argument pages are charged to the owning process's quota
  before they are allocated.
- A per-image page cap bounds any single load regardless of quota.

### 4. W^X

x86-32 without PAE has no per-page execute bit, so a writable page is executable
whether or not anyone intended it. A section that is both writable and executable
is therefore refused **at load**, which is the only place the rule can be
enforced at all.

### 5. Capabilities

A process gets only the capabilities it was granted. A way to perform an
operation without its capability — disk access without `GRANT_DISK`, for example
— is a vulnerability.

### 6. Key material

The platform signing key's private half (`*.xksk`) is the root of all trust in a
CXOS system: anyone holding it can sign a kernel that an installed machine will
load. It is **never** committed, and neither is `*.xusk`, `boot/uefi/sbkeys/`,
`*.pfx`, `*.pem`, `*.cer`, `*_VARS.fd` or `*.signed.efi`. Only
`tools/kernel.xkpk`, the public half, is tracked.

**A commit containing a private key is a vulnerability even if the branch is
deleted afterwards**, because the object remains reachable. Report it, and treat
the key as compromised and rotate it.

## What is *not* a security boundary

Reports about these are not vulnerabilities. They are documented behaviour, and
knowing them saves everyone time.

**Development kernels run unsigned code on purpose.** A kernel built with
`cxk os build --dev` (`CXK_DEV_UNSIGNED`) admits images with no signature at all.
That is its entire reason for existing, it announces itself at boot, and it must
never be shipped. "A dev kernel ran my unsigned binary" is the feature working.

**The release disk image attests to nothing.** `cxos-<version>-<name>-selfsigned.img`
is signed by a key generated for that one build and destroyed afterwards. The
image verifies itself end to end, and that is all it claims — it is not signed by
the CXOS platform key. Platform-key signing is an open decision
(`docs/planning/HARDENING_PLAN.md` §5).

**Physical access is out of scope.** There is no disk encryption, no measured
boot and no Secure Boot enforcement. Anyone who can write to the disk can replace
the kernel. The UEFI boot chain is a stub.

**The DevKit is a developer tool.** `cxk` and CX DevKit Studio process files the
developer chose to open. Crashes and parser failures on malformed input are bugs
worth reporting and are covered by an adversarial test suite, but the trust
boundary that matters is the kernel's — it validates everything independently and
assumes nothing about what produced an image.

## Known gaps

Current, deliberate, and written down so nobody has to discover them by
surprise.

| Gap | Status |
|---|---|
| **`cxex_check_compat` has no caller.** Every image sets `FLAG_REQUIRE_ABI_MATCH` and declares an `abi_version`, and nothing compares them. The arch and ABI match images ask for is not enforced. | Tracked in `docs/planning/VERSIONING_AND_RELEASE.md` |
| **No ASLR.** Images load where they ask to. | Not planned for 1.x |
| **No per-page NX.** x86-32 without PAE; W^X at load is the substitute. | Architectural |
| **No Secure Boot, no measured boot, no disk encryption.** UEFI is a stub. | Roadmap |
| **No automated security checks in CI.** GitHub Actions does not run on this repository, so nothing is scanned or built automatically. Every build to date has been on one developer machine. | Billing, see `VERSIONING_AND_RELEASE.md` §5 |
| **Linux and macOS builds are unverified.** The `cxk-linux-x64` and `cxk-osx-arm64` binaries are cross-published from Windows and have never been executed on their target platforms. | Blocked on the above |

## How this gets tested

Claims above are not taken on trust:

- **`ktest.c` runs at every boot** and the kernel is not considered working until
  it logs `self-tests: all N passed`. Twelve of those cases are adversarial loader
  tests driven through mock ops — each a single mutation of a known-good image
  that is itself a case, and each refusal also asserts that *nothing was mapped*,
  because a loader that refuses after mapping half an image has still placed the
  attacker's pages.
- **Every check is paired with a sabotage run**: delete the check, rebuild, and
  require exactly the tests that cover it to fail. Several tests have been caught
  passing for the wrong reason this way.
- **`CXEX.Tests`** carries adversarial ELF, CXEX and crypto suites on the DevKit
  side.

The two 2026-10-01 security reviews (`docs/reviews/2026-10-01/`) and the response
to them (`docs/planning/HARDENING_PLAN.md`) are kept as written, including the
findings that are still open.

## Licence

CXOS is **not** open source. See [LICENSE.md](LICENSE.md). Reporting a
vulnerability does not grant any licence to the Project, and nothing here
authorises redistribution, modification or reverse engineering beyond what that
licence permits.
