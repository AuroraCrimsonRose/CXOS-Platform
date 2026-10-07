# Security policy

CXOS is an operating system, so most of what it does is a security boundary of
some kind. This document says which boundaries are real, which are deliberately
absent, and how to report something that crosses one.

Where a protection does not exist yet, it is named here rather than left out.

## Reporting a vulnerability

### → [Report a vulnerability](https://github.com/AuroraCrimsonRose/CXOS-Platform/security/advisories/new)

Or: **Security → Advisories → Report a vulnerability**, from any page of this
repository.

That opens a draft advisory visible only to you and the maintainer. Nothing
becomes public while a fix is being written, you keep access to the thread, and a
CVE can be requested from it if one is warranted. No email address is involved on
either side. A published contact address is a permanent target, and this channel
does not need one.

Please do not open an issue or a pull request for a vulnerability, and please do
not discuss it publicly until a fix is released.

> **If that link 404s**, private vulnerability reporting is not enabled yet, or
> the repository is still private. It is a public-repository feature.
>
> In that case, start a
> [discussion](https://github.com/AuroraCrimsonRose/CXOS-Platform/discussions)
> titled "security contact request". Say only that you have something to report.
> Do not say what it is. You will be contacted privately.
>
> That is a poor substitute and it is temporary. If you are reading this and the
> link still 404s, the repository is not finished being set up.

Please include:

| | |
|---|---|
| Version | `cxk --version`, or the kernel banner at boot |
| Build type | Release or development (`cxk os build --dev`). This changes what counts as a bug. |
| Reproduction | The smallest input or steps that trigger it |

A CXEX image, disk image or X source that reproduces the problem is worth more
than a description of one.

**What to expect.** CXOS is maintained by one person. There is no on-call
rotation and no guaranteed response time. Reports are read. A fix for a real
boundary crossing takes priority over everything else in the plan.

Please do not test against infrastructure you do not own. There is no bug bounty.

## Supported versions

| Version | Supported |
|---|---|
| 1.0.x | Yes |
| < 1.0 | No. Pre-release, never published. |

One line is supported at a time. CXOS makes no backwards-compatibility promise.
Formats, the ABI and interfaces change freely between releases, and every user of
a changed interface is updated in the same commit. Fixes ship as a new release,
not as a patch to an old one.

## What is a security boundary

These are the claims CXOS makes. A way to break one is a vulnerability.

### 1. Ring 3 cannot reach the kernel

A loaded image runs in ring 3. It must not be able to read, write or execute
kernel memory, or get the kernel to do so for it.

Every section of an image must lie wholly below `KERNEL_VBASE`. The check is done
in 64-bit arithmetic so the sum cannot wrap back into range.

`paging_map_user` refuses any address at or above `KERNEL_VBASE`.
`paging_map_kernel` refuses `PAGE_USER`. Neither entry point can express a
ring-3-reachable kernel mapping, so no caller has to remember not to ask for one.

Every syscall pointer is checked with `user_ptr_readable` or `user_ptr_writable`,
whichever matches the access actually performed. Ring 0 ignores the read-only
page bit unless CR0.WP is set, so "present and user" does not justify a write.

### 2. Only verified code runs

On a release kernel, an image runs only if it carries a valid CXSG signature over
its bytes, made by the key compiled into the kernel as its root of trust.

Everything the signature covers ends at `signature_offset`. A section whose file
bytes reach past that point is refused, because those bytes are not signed.

The kernel validates independently of the DevKit. Both sides check. Neither
trusts the other's check.

### 3. An image cannot exhaust the machine

The loader validates an image completely before allocating a single frame, so a
refusal costs nothing.

Image, stack and argument pages are charged to the owning process's quota before
they are allocated. A separate per-image page cap bounds any single load
regardless of quota.

### 4. W^X

x86-32 without PAE has no per-page execute bit, so a writable page is executable
whether anyone intended it or not. A section that is both writable and executable
is refused at load. That is the only place the rule can be enforced at all.

### 5. Capabilities

A process gets only the capabilities it was granted. Performing an operation
without its capability, such as disk access without `GRANT_DISK`, is a
vulnerability.

### 6. Key material

The platform signing key's private half (`*.xksk`) is the root of all trust in a
CXOS system. Anyone holding it can sign a kernel that an installed machine will
load.

It is never committed. Neither is `*.xusk`, `boot/uefi/sbkeys/`, `*.pfx`,
`*.pem`, `*.cer`, `*_VARS.fd` or `*.signed.efi`. Only `tools/kernel.xkpk`, the
public half, is tracked.

A commit containing a private key is a vulnerability even if the branch is
deleted afterwards, because the object stays reachable. Report it. Treat the key
as compromised and rotate it.

## What is not a security boundary

These are documented behaviour. Knowing them saves everyone time.

**Development kernels run unsigned code on purpose.** A kernel built with
`cxk os build --dev` (`CXK_DEV_UNSIGNED`) admits images with no signature at all.
That is the entire reason it exists. It announces itself at boot and must never
be shipped. "A dev kernel ran my unsigned binary" is the feature working.

**The release disk image attests to nothing.**
`cxos-<version>-<name>-selfsigned.img` is signed by a key generated for that one
build and destroyed afterwards. The image verifies itself end to end, and that is
the only claim it makes. It is not signed by the CXOS platform key. Platform-key
signing is still an open decision (`docs/planning/HARDENING_PLAN.md` §5).

**Physical access is out of scope.** There is no disk encryption, no measured
boot and no Secure Boot enforcement. Anyone who can write to the disk can replace
the kernel. The UEFI boot chain is a stub.

**The DevKit is a developer tool.** `cxk` and CX DevKit Studio process files the
developer chose to open. Crashes and parser failures on malformed input are real
bugs and an adversarial suite covers them, but the trust boundary that matters is
the kernel's. It validates everything itself and assumes nothing about what
produced an image.

## Known gaps

| Gap | Status |
|---|---|
| **`cxex_check_compat` has no caller.** Every image sets `FLAG_REQUIRE_ABI_MATCH` and declares an `abi_version`, and nothing compares them. The arch and ABI match that images ask for is not enforced. | Tracked in `VERSIONING_AND_RELEASE.md` |
| **No ASLR.** Images load where they ask to. | Not planned for 1.x |
| **No per-page NX.** x86-32 without PAE. W^X at load is the substitute. | Architectural |
| **No Secure Boot, measured boot or disk encryption.** UEFI is a stub. | Roadmap |
| **No automated security checks in CI.** GitHub Actions does not run on this repository, so nothing is scanned or built automatically. Every build so far has been on one developer machine. | Billing. See `VERSIONING_AND_RELEASE.md` §5 |
| **Linux and macOS builds are unverified.** `cxk-linux-x64` and `cxk-osx-arm64` are cross-published from Windows and have never been run on their target platforms. | Blocked on the above |

## How the claims are tested

**`ktest.c` runs at every boot.** A kernel change is not finished until a boot
logs `self-tests: all N passed`. Twelve of those cases are adversarial loader
tests driven through mock ops. Each one is a single mutation of a known-good
image, and the known-good image is itself a case, so the suite cannot pass by
refusing everything. Each refusal also asserts that nothing was mapped, because a
loader that refuses after mapping half an image has still placed the attacker's
pages.

**Every check is paired with a sabotage run.** Delete the check, rebuild, and
require exactly the tests covering it to fail. Several tests have been caught
passing for the wrong reason this way.

**`CXEX.Tests`** carries adversarial ELF, CXEX and crypto suites on the DevKit
side.

The two 2026-10-01 security reviews (`docs/reviews/2026-10-01/`) and the response
to them (`docs/planning/HARDENING_PLAN.md`) are kept as written, including the
findings still open.

## Licence

CXOS is not open source. See [LICENSE.md](LICENSE.md). Reporting a vulnerability
grants no licence to the Project, and nothing here authorises redistribution,
modification or reverse engineering beyond what that licence permits.
