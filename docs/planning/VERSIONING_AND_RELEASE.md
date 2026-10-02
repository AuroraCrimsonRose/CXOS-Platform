# Versioning and releases

How every version in this repository is decided, where it is written, what makes
it true, and how a release is cut. This is engineering review §13 ("version and
format compatibility should be centralized") made concrete.

The one rule: **`versions.json` at the repository root decides. Everything else
is checked against it.** If a number disagrees with the registry, the number is
wrong — not the registry.

---

## 1. Two kinds of version, which do not work the same way

**Components** ship. Each carries its own semver and moves at its own pace. A
release bundles particular component versions; it does not force them to agree.

**Formats** are on-disk and on-wire layouts. Each is a single monotonic integer,
never reset, never tied to a component version. They exist separately precisely
because a reader can always be older than the file it is handed — which is the
one place in this repository where two different ages do meet, since `cxk` from a
release may be pointed at an image or a key written by a different one.

Everything else in CXOS is held to one age by `cxk check-abi` and by CLAUDE.md's
"update every user in the same commit". Format versions are the exception, so
they are the thing that actually needs numbering.

## 2. The components

| Component | What it is | Ships as |
|---|---|---|
| **CXOS** | The platform release: kernel, X userland and boot chain as one bootable image. Its version names the **git tag** and the **GitHub Release**. | `cxos-<version>-<name>-selfsigned.img` |
| **CXK** | The kernel. | inside the image |
| **cxk CLI** | The X compiler, the imager, signing, `cxk os build`, `cxk run`. | `cxk-win-x64.exe`, `cxk-linux-x64`, `cxk-osx-arm64` |
| **CX DevKit Studio** | The GUI over the same libraries the CLI uses. | `CXEX-Studio-<platform>.zip` |
| **X Native VSIX** | The X language extension for VS Code. | VS Code marketplace, tag `vsix-v*` |

All five start at **1.0.0**. 1.0.0 is the first version anyone outside the
project can obtain, which is the only thing a public version number can honestly
be about.

> **Why not 5.0.0.** The kernel called itself `v5` internally — the boot banner,
> the `CXK_v5` CMake project, `dist/CXK_x86_32`, four NASM banner strings. None of
> those generations was ever released. Shipping 5.0.0 would have claimed four
> releases that never existed, to an audience with no way to find out what they
> were. The generation counter is retired: the names that carried it have had it
> **removed rather than renumbered**, because a version repeated in five
> hand-written strings is five things to forget — exactly the failure this
> registry exists to stop. The kernel banner now reads its version from the
> registry, and it is the only place a CXK version is printed.

### What a bump means

| | |
|---|---|
| **MAJOR** | A new generation of that component — it is a different thing, not an update to the previous one. **Not** keyed to breaking changes: CLAUDE.md says compatibility is never a constraint and formats may change freely, so a major keyed to breakage would increment every release and carry no information at all. |
| **MINOR** | New capability. This is where format, ABI and interface changes land, and that is expected. |
| **PATCH** | Fixes only. No interface, format or ABI change. |

No pre-release suffixes and no build metadata: the VS Code marketplace accepts
only a bare `major.minor.patch`, so keeping every component to three integers
means one string works everywhere.

Because a component version carries no compatibility promise, it is a statement
about an **artifact**, not a migration burden. `cxk 1.2.0` tells you which `cxk`
you have; the image it can build is whichever one shipped alongside it.

### Major release names

Every **major CXOS release** carries a Greek mythological name after the version:
`CXOS 1.0.0 "Hekate"`.

> **The semantic version identifies the release. The name identifies what that
> major generation *means*.**

**The name belongs to the CXOS release and to nothing else.** Not the kernel, not
the boot chain, not the tooling: CXK, the BIOS/UEFI boot chain, the `cxk` CLI, CX
DevKit Studio and the X Native VSIX all have versions and **no names**. They are
components, and a component is a part of a generation rather than a generation in
itself.

So there is no "CXK Hekate" and no "stage 2 Hekate". The kernel shipped in
CXOS 1.x is **CXK 1.0.0**, and that is the whole of what it is called. The name
appears on the OS release — the tag, the GitHub Release, the disk image and the
documentation that describes that generation.

It is not a codename and it is not decorative — it is meant to become part of
CXOS's historical identity, which is why it is chosen from the architecture rather
than from a list of figures that sound impressive.

**A name is never assigned automatically, and is not official until Aurora has
approved it.** For each new major:

1. Analyse the defining architectural changes, goals and milestones of that
   generation.
2. Identify several candidates — gods, Titans, primordials or other significant
   figures — whose symbolism meaningfully corresponds to those changes.
3. Recommend one, stating the figure and their domain, how the symbolism maps to
   the release's architectural identity, why it suits *this* generation
   specifically, and the meaningful alternatives considered.
4. Wait for approval, then use it consistently in the release and its
   documentation.

There is **no predetermined sequence** and no fixed mapping. Hephaestus (forging,
tools), Athena (wisdom, strategy), Hermes (messages, movement), Prometheus
(knowledge, independence), Atlas (foundations, scale) and Hestia (stability,
centrality) are associations to reason from, not a menu to be drawn from in order.
Judge each generation on what it actually accomplished.

Minor and patch releases inherit their major's name; they do not get their own.
`versions.json` holds the approved name under `components.cxos.release_name`, and
`cxk check-versions` holds the table below to it.

| Major | Name | Why |
|---|---|---|
| CXOS 1.x | **Hekate** | Goddess of thresholds, gateways and crossroads, whose epithet *Kleidophoros* means **key-bearer**: she is shown holding the keys to the gate she guards, belonging to neither side of the boundary but to the boundary itself. CXOS 1.x is the generation in which the platform gained a guarded boundary and the keys to it — the loader validates an image whole before any of it is mapped, a section's range must lie below `KERNEL_VBASE`, `paging_map_kernel` and `paging_map_user` are two gates deliberately shaped so neither can express the dangerous combination, W^X decides what may execute once across, and every byte must lie inside the signed range. The keys are not a metaphor: this is where the signed chain first verified end to end, with the kernel's root of trust generated from the key that signed what it loads. CXK already booted, scheduled, paged and ran programs before this; what it *gained* was the authority to refuse, and a reason to be believed when it does.<br><br>Alternatives weighed: **Themis** (divine law and right order, and keeper of the Delphic oracle before Apollo — apt, but about judgment in general, and better suited to a later generation about capability tiers and policy); **Atlas** (foundations and scale — describes the kernel's existence rather than this generation's work, and his burden is a punishment). Rejected: **Hephaestus** (the forge fits the DevKit, but tooling was the means here, not the identity — hold it for a generation whose subject *is* the toolchain, such as the assembler and linker in X); **Prometheus** (fits a first release and self-hosting, but his character is defiance of limits, the opposite of this generation); **Hestia** (first of every offering, but too quiet for work whose whole content is refusal). |

## 3. The formats

| Format | Version | What it describes | Declared in |
|---|---|---|---|
| **CXEX** | 1 | The executable container: 56-byte header, 28-byte section entries | `kernel/lib/format/cxex.h` `CXEX_FORMAT_VERSION`, `CXEXExecutable.SupportedFormatVersion` |
| **ABI** | 2 | Syscall numbers, struct layouts, capability meanings. Stamped into every image's `abi_version`. | `abi/cxk_abi.h` `CXK_ABI_VERSION`, `CXEXLayoutEngine.AbiVersion` |
| **CXBI** | 1 | The boot-information block stage 2 hands the kernel | `abi/cxk_boot.h` `CXBI_VERSION` |
| **CXFS** | 2 | The filesystem on-disk layout | `kernel/drivers/storage/filesys/cxfs.h` `CXFS_VERSION` |
| **XBPT** | 1 | The partition table: where the boot chain, packages and filesystem sit | `kernel/drivers/storage/partition/partition.h` `XBPT_VERSION` |
| **XSTG** | 2 | The install manifest: 16-byte header, 48 bytes per file | `kernel/drivers/storage/install.h` `XSTG_VERSION`, `XBPTImageWriter.XstgVersion` |
| **XKPK** | 1 | The CXPK public key container | `CXKeyGenerator.XKPK_VERSION` |
| **CXSG** | — | The signature block | **No version field, by design**: described by its `sig_algo` and `hash_algo` identifiers instead, which say what to do rather than what generation it is. Recorded in the registry with version `0` and no checks so the set is complete and nobody wonders whether one was forgotten. |

A format version bumps **in the commit that changes the format, on every side of
it at once.** `XBPT_VERSION` is not the `5` that the imager used to print — that
was the kernel generation, and it is gone.

## 4. What makes the registry true

Every entry in `versions.json` may carry `checks`: a file and a regex whose first
capture group must equal the declared version.

```json
"cxfs": {
  "version": 2,
  "checks": [
    { "file": "kernel/drivers/storage/filesys/cxfs.h",
      "pattern": "#define CXFS_VERSION +([0-9]+)" }
  ]
}
```

`cxk check-versions` enforces all of them, and `cxk os build` runs it as a
pre-flight alongside `cxk check-abi`. It is deliberately **data-driven**: a new
place a version is written is a new entry in `versions.json` and no change to the
checker. A checker that had to be edited to learn about a new site is a checker
that stops covering the sites nobody remembered to add.

Three failure modes, all of them hard failures:

- **MISMATCH** — the file says something other than the registry.
- **NOT FOUND** — the pattern matched nothing. This fails rather than passing,
  because a check that silently matches nothing reports green for a file that may
  say anything at all. That is how a registry rots.
- **MISSING / BAD PATTERN** — the file or the regex is gone.

Each has been verified to fire.

### Where each build system reads it

| | |
|---|---|
| **Kernel** | `tools/cmake/CMakeLists.txt` reads `versions.json` with `string(JSON …)` and generates `build/cxk_version.h`. A generated header, not a `-D` — the quotes a C string needs do not survive into a custom-command argument list, so the define arrived unquoted and the banner failed to compile. |
| **C# (16 projects)** | `devkit/Directory.Build.props` holds `CxkCliVersion` and `CxexStudioVersion`, because MSBuild has no JSON reader. Held to the registry by `check-versions`. The CLI reads its own assembly version rather than carrying a string. |
| **VSIX** | `editors/vscode/package.json`, checked directly. |

## 5. Cutting a release

> **GitHub Actions does not run on this repository.** Every run since the workflow
> was added is a `startup_failure` at 0 seconds, on tags and branch pushes alike:
> *"The job was not started because recent account payments have failed or your
> spending limit needs to be increased."* The repository is private, so runs
> consume paid minutes. `release.yml` itself is sound — js-yaml parses it,
> `actionlint` reports nothing, the committed blob is valid UTF-8 — and it is kept
> for the day Actions is enabled. **Until then releases are built locally**, and
> the workflow's claim to be "the only continuous check that D5's one toolchain on
> every host is true" is **not** currently true of anything.

1. Decide the new version in `versions.json`. That is the only file a human edits
   for a version.
2. `cxk check-versions` — or just `cxk os build`, which runs it.
3. Commit, push to `x86_32_DEV`.
4. Tag `v<CXOS version>` and push the tag. For the VSIX alone, `vsix-v<version>`.
5. Build the assets and attach them (below). `release.yml` would do this step
   if Actions were available; the artifacts and their names are identical either
   way, deliberately, so enabling Actions later changes nothing a user sees.

### Building the assets locally

The same six binaries, the same image, the same checksum file. .NET
cross-publishes all three RIDs from any host, and CXEX Studio is Avalonia rather
than WPF, so it genuinely builds for Linux and macOS too.

```
for RID in win-x64 linux-x64 osx-arm64; do
  dotnet publish devkit/CXEX.CLI -c Release -r $RID \
    -p:SelfContained=true -p:PublishSingleFile=true \
    -p:IncludeNativeLibrariesForSelfExtract=true -p:DebugType=none \
    -o build/rel/cli-$RID
  dotnet publish devkit/CXEX.Studio -c Release -r $RID \
    -p:SelfContained=true -p:DebugType=none -o build/rel/studio-$RID
done

cxk keygen tools/rel && cxk os build --key rel --clean && rm tools/rel.xksk
```

Name the binaries `cxk-<rid>[.exe]`, zip each Studio directory as
`CXEX-Studio-<rid>.zip`, copy the image to
`cxos-<version>-<name>-selfsigned.img`, and `sha256sum * > SHA256SUMS`.

**Delete the private half of the signing key when the build finishes.** That is
not tidiness — it is the property the asset's name claims: the image verifies
itself end to end and the key behind it can sign nothing for anyone else's
machine. In CI the runner's destruction did this; locally it has to be done on
purpose.

Attach them with `gh release create v<version> --title 'v<version> "<Name>"'`,
or through the Releases page.

### What the workflow produces

| Asset | From |
|---|---|
| `cxk-win-x64.exe`, `cxk-linux-x64`, `cxk-osx-arm64` | `publish` job, single-file self-contained |
| `CXEX-Studio-<platform>.zip` | `publish` job |
| `cxos-<version>-<name>-selfsigned.img` | `image` job, on a clean ubuntu runner with apt clang/lld/nasm/ninja/cmake |
| `SHA256SUMS` | `release` job |

The release body is written in the workflow and lists what each asset is. The
auto-generated commit notes are appended to it.

### The image's signatures

The `image` job generates a key, signs every artifact with it, and the key is
destroyed with the runner. The image therefore verifies itself end to end — and
attests to nothing, because the key behind it can sign nothing for anyone else's
machine. It is a test image.

This matters because of what it replaced: the job used to build with no key and
without `--dev`, which is a **release** kernel (one that requires signatures)
packaged with a userland that has none. It booted, passed its self-tests, and
then refused its own executive. CMake had been saying so the whole time —
*"executive.xoex UNSIGNED — the kernel WILL REFUSE to launch it."*

An image signed by the **platform** key waits on the release-signing decision in
`HARDENING_PLAN.md` §5, which is now a policy question only: local signing versus
a CI secret.

### Keys

`--key <name>` selects `tools/<name>.xksk` + `tools/<name>.xkpk`. One name for
both halves, never two paths: they must be halves of the same pair, and `CXSigner`
refuses to sign when they are not. The kernel's root of trust is generated from
the public half of whichever key signs, **every build**, so the key the kernel
trusts is always the key the build signed with. Embed one and sign with the other
and every artifact is refused at boot as `BAD_SIGNATURE` — which reads as
tampering rather than as the wrong key on a command line.

Only `tools/kernel.xkpk` is tracked. Any other pair in `tools/` is gitignored: a
local test pair is local to one machine, since its private half can never be
committed.

## 6. What this registry found

The registry was not bookkeeping. Writing it surfaced four live inconsistencies,
none of which any build could previously have caught:

1. **Three component versions, no two agreeing.** The CLI printed a hardcoded
   `5.0.0`, the VSIX said `0.5.0`, and all 16 C# projects reported `1.0.0` because
   nothing set `<Version>` in any of them.
2. **The kernel never validated CXEX `format_version`.** The DevKit refuses a
   version it does not know and has a test for it; the kernel read the field and
   checked nothing — leaving the permissive side the one an untrusted image
   actually reaches. Both sides validating independently is the rule this
   repository holds itself to (security review §11). **Fixed:** the kernel refuses
   a container layout it does not know.
3. **Every image was stamped `abi_version = 1` against a contract documented as
   v2** — and every image sets `FLAG_REQUIRE_ABI_MATCH`, asking to be refused on
   exactly that mismatch. It survived because **`cxex_check_compat` has no
   caller**: nothing ever compared the two. `CXK_ABI_VERSION` now exists in
   `abi/cxk_abi.h`, the DevKit stamps 2, and the registry holds both to it.
4. **XSTG's version existed only in a comment** — "the XSTG manifest (version 2,
   CXK install.h)" — so "the kernel says 2" was a claim no build could check. It is
   a constant on both sides now.

### Still outstanding

- **`cxex_check_compat` has no caller.** The arch and ABI match every image asks
  for is not enforced. Wiring it into the load path is a behaviour change that
  needs its own boot verification, and getting it wrong refuses every image — so
  it is tracked here rather than done quietly alongside a renumbering.
