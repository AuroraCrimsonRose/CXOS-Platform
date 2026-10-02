# Contributing to CXOS

**Read this before writing any code.** CXOS is not open source, and the terms
below are not boilerplate — they change what happens to work you submit.

## Who may contribute

Access to the source of the Proprietary Components (CXK, CXOS, and variants) is
granted **only to individuals or entities authorised in writing by CATX Systems
LLC** — an *Authorized Contributor* under [LICENSE.md](LICENSE.md) §8.

- Possession of the source grants no right of redistribution, public display or
  public forking (§8c).
- Authorized Contributor status may be revoked at any time, and on revocation you
  must destroy your copies of the proprietary source (§8d).
- If you are not authorised, you have no rights under the licence (§8e), and an
  unsolicited pull request cannot be accepted.

If you want to contribute, ask first: **aurora.tejeda@catxhosting.com**.

## What submitting a contribution does

Under [LICENSE.md](LICENSE.md) §9, by submitting a contribution you grant CATX
Systems a perpetual, worldwide, irrevocable licence to it — and **on acceptance
into a Proprietary Component, all right, title and interest in that contribution
is assigned to CATX Systems LLC.** You keep no ownership of merged work.

You must be legally entitled to grant that (§9c). If your employer owns what you
write, you need their sign-off before submitting.

You may apply *Basic Concepts* you learn from the source in independent projects,
provided you take no proprietary source or trade secrets with you (§9d).

The **SDK** components are MIT (§10), and applications, drivers and extensions
you build with the SDK are yours to license as you choose (§11, §12).

## Security issues are not pull requests

Do not open an issue or a PR for a vulnerability. See [SECURITY.md](SECURITY.md).
A fix sent as a public PR discloses the bug before anyone can update.

## Working agreements

These are the rules the repository actually enforces. They are in
[CLAUDE.md](CLAUDE.md) in full; the ones that most often catch people out:

**Branches.** `x86_32_DEV` is master — work goes there. `x86_32_RELEASE` is
releases only, by PR. Don't create per-feature branches unless asked.

**Never commit key material.** `*.xksk`, `*.xusk`, `boot/uefi/sbkeys/`, `*.pfx`,
`*.pem`, `*.cer`, `*_VARS.fd`, `*.signed.efi`. Only `tools/kernel.xkpk`, the
public half, is tracked. A private key in a commit is a security incident even if
the branch is deleted, because the object stays reachable.

**Never commit build output.** `build/`, `dist/`, `os/executive/app_image.h`,
`bin/`, `obj/`. Anything a build writes belongs in `.gitignore`.

**ABI and version changes are one commit.** A syscall or struct added to
`abi/cxk_abi.h` is added to `devkit/CXEX.Lang/Abi/AbiPrelude.cs` in the same
change, and `cxk check-abi` must pass. Versions are decided in `versions.json`
and nowhere else — `cxk check-versions` will fail the build if a number in the
tree disagrees with it. See
[docs/planning/VERSIONING_AND_RELEASE.md](docs/planning/VERSIONING_AND_RELEASE.md).

**The C# compiler is the oracle** while `os/xc` matches it. A change to either
compiler keeps the differential tests and the self-hosting fixed point identical.

**Don't skip, weaken or delete a test to get green.**

## What "done" means

A change is not finished because it compiles.

- **Kernel changes must boot.** `ktest.c` runs at every boot; the change is not
  done until a boot logs `self-tests: all N passed`. Watch for CPU exceptions too
  — `qemu -d int` and check that only the expected vectors appear.
- **New checks must be seen to fail.** Delete the check, rebuild, and confirm
  that exactly the tests covering it go red. Several tests in this repository
  have been caught passing for the wrong reason — a mutation that was a no-op, a
  builder that sized a file around the field under test, a disk test silently
  skipped on the wrong machine type. A test that has never failed is not evidence.
- **Run the checks:** `cxk check-abi`, `cxk check-versions`,
  `cxk check tools/cmake/CMakeLists.txt`, and `dotnet test devkit/CXEX.Tests`.
  `cxk os build` runs the first three as pre-flights.

## Building

```
dotnet build devkit/CXEX.Studio.slnx -c Release     # the DevKit
cxk os build --dev                                  # the OS, development kernel
cxk run dist/CXK_x86_32/images/cxk_disk.img         # boot it
```

Needs clang, ld.lld, NASM, CMake and Ninja on PATH. No i686-elf GCC, no NMake.
Full detail in [CLAUDE.md](CLAUDE.md).

## Style

Match the surrounding code — its naming, its idiom, its comment density.

Comments here explain **why**, and especially why something that looks wrong is
right: the bug that motivated a check, the failure a line prevents, the reason an
obvious simplification is unsafe. A comment restating what the code plainly does
is noise; a comment recording the hour someone lost is worth keeping.
