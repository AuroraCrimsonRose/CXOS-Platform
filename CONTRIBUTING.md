# Contributing to CXOS

## CXOS is not accepting outside contributions

**Please do not open a pull request. It cannot be merged, however good it is.**

This is a deliberate closure with a date on it, not an opinion about anyone's
code. Contributions will open **once a legal entity exists to receive them**;
CATX Systems is presently a sole proprietorship, so there is no company to hold
the rights to contributed work and no counterparty to an agreement about it.
Merging someone else's copyright into the project before that is settled creates
a tangle that is painful to unwind and unfair to the contributor.

When it opens, this file will say so and will state what is required.

**If you have found a bug**, or want to propose something, please open an
[issue](https://github.com/AuroraCrimsonRose/CXOS-Platform/issues) instead. A
clear report is genuinely more useful right now than a patch, because the report
can be acted on and the patch cannot.

**If you have found a vulnerability**, it goes to the private channel in
[SECURITY.md](SECURITY.md), not to an issue or a PR.

## What you *can* do today

The MIT tier (`abi/`, `os/std/`, `os/xc/`, `devkit/`, `editors/vscode/`,
`tools/`, `docs/`) is yours under [MIT](LICENSES/MIT.txt). Fork it, modify it,
build on it, ship it commercially; nothing is owed.

**Applications, drivers and extensions you write for CXOS are independent
works** and you may license and sell them however you like. See
[LICENSE.md](LICENSE.md) for the exemption, stated in full.

The operating-system tier is
[PolyForm Noncommercial 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.txt): read
it, audit it, build it, modify it and share it for any noncommercial purpose.

## Security issues are not pull requests

Do not open an issue or a PR for a vulnerability. See [SECURITY.md](SECURITY.md).
A fix sent as a public PR discloses the bug before anyone can update.

## Working agreements

These are the rules the repository actually enforces, most of them by a check
that fails the build, not by convention. The ones that most often catch people
out:

**Branches.** `x86_32_DEV` is master; work goes there. `x86_32_RELEASE` is
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
and nowhere else. `cxk check-versions` fails the build if a number in the
tree disagrees with it. See
[docs/planning/VERSIONING_AND_RELEASE.md](docs/planning/VERSIONING_AND_RELEASE.md).

**The C# compiler is the oracle** while `os/xc` matches it. A change to either
compiler keeps the differential tests and the self-hosting fixed point identical.

**Don't skip, weaken or delete a test to get green.**

## What "done" means

A change is not finished because it compiles.

- **Kernel changes must boot.** `ktest.c` runs at every boot; the change is not
  done until a boot logs `self-tests: all N passed`. Watch for CPU exceptions
  too: run `qemu -d int` and check that only the expected vectors appear.
- **New checks must be seen to fail.** Delete the check, rebuild, and confirm
  that exactly the tests covering it go red. Several tests in this repository
  have been caught passing for the wrong reason: a mutation that was a no-op, a
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
**clang 23 or newer** — not negotiable, and newer than most distributions ship:
clang 18 through 22 ignore `-fuse-ld=lld` for `--target=i686-elf` and try to link
through `gcc`, which fails on a host that has none. The CI container pins it. `cxk os build` pre-flights the toolchain and
names whatever is missing rather than failing somewhere inside CMake.

Or build in the CI container, which pins the whole toolchain:

```
docker build -f tools/ci/Dockerfile -t cxos-ci .
docker run --rm -v "$PWD:/src" -w /src cxos-ci tools/ci/build.sh
``` Release-side detail is in
[docs/planning/VERSIONING_AND_RELEASE.md](docs/planning/VERSIONING_AND_RELEASE.md).

## Style

Match the surrounding code: its naming, its idiom, its comment density.

Comments here explain **why**, and especially why something that looks wrong is
right: the bug that motivated a check, the failure a line prevents, the reason an
obvious simplification is unsafe. A comment restating what the code plainly does
is noise; a comment recording the hour someone lost is worth keeping.
