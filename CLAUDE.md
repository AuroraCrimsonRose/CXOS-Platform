# CXOS Platform — working notes

One repository for CXOS: **CXK** (the kernel), the **X userland** (including
`os/xc`, the X compiler written in X), the boot chain, and the **CX DevKit**
(`devkit/`: the C# X compiler, the `cxk` CLI, signing, imaging, CXEX Studio).
The owner holds the only copy, so backwards compatibility is never a
constraint. Change formats and interfaces freely, and update every user in the
same commit.

## Branches

- **`x86_32_DEV`**: active development. Commit and push here directly. Do
  **not** create per-session or feature branches unless asked.
- **`x86_32`**: full releases only. Never push to it unless asked.
- A `v*` tag runs `.github/workflows/release.yml`, which publishes `cxk` and
  CXEX Studio (win-x64, linux-x64, osx-arm64) to a GitHub Release.
- Open a PR only when asked.

## Layout

| Path | |
|---|---|
| `abi/` | `cxk_abi.h`, `cxk_boot.h`: the ABI both sides compile against |
| `boot/` | BIOS boot chain (NASM); `uefi/` stub |
| `kernel/` | CXK; `kernel/ktest.c` holds the boot-time self-tests |
| `os/` | X userland: `apps/`, `std/`, `xc/`, `executive/`, `config/`, `services/` |
| `tools/` | `cmake/` (the build), `kernel.xkpk` (platform public key), scripts |
| `devkit/` | `CXEX.*` C# projects, `CXEX.Studio.slnx`, `tests/` |
| `editors/vscode/` | X language extension |
| `docs/` | By topic; index at `docs/README.md` |

## Rules that are never broken

- **Never commit key material:** `*.xksk`, `*.xusk`, `boot/uefi/sbkeys/`,
  `*.pfx`, `*.pem`, `*.cer`, `*_VARS.fd`, `*.signed.efi`. `tools/kernel.xkpk`
  is the public key and is tracked; its private half `tools/kernel.xksk`
  never is.
- **Never commit generated output:** `build/`, `dist/`,
  `os/executive/app_image.h`, `kernel/lib/format/trusted_key.c`, `bin/`,
  `obj/`, `docs/devkit/_site/` and `docs/devkit/api/`. Anything a build writes
  goes in `.gitignore`.
- **ABI changes are one commit:** a syscall or struct added to
  `abi/cxk_abi.h` is added to `devkit/CXEX.Lang/Abi/AbiPrelude.cs` in the same
  change. `cxk check-abi` must pass.
- **The C# compiler is the oracle** while `os/xc` matches it
  (`docs/language/CX_X_CORE_LANG.md` §10). A change to either compiler keeps the
  differential tests and the self-hosting fixed point identical.
- Don't skip, weaken or delete a test to get green.

## Build

DevKit (any OS, .NET 10):

```
dotnet build devkit/CXEX.Studio.slnx -c Release
```

The OS build needs `cxk` in `tools/` (`tools/cxk.exe` on Windows). Take it from
a release, or `dotnet publish devkit/CXEX.CLI -c Release -r <rid>
-p:SelfContained=true -p:PublishSingleFile=true`. `trusted_key.c` is not
tracked; generate it once:

```
cxk embed tools/kernel.xkpk kernel/lib/format/trusted_key.c cxos_trusted_key --extern
```

- **Every host:** `cxk os build` (signed if `tools/kernel.xksk` exists) or
  `cxk os build --dev` (development kernel: runs unsigned programs, never ship
  it). It pre-flights the toolchain, the source list and the ABI, then
  configures CMake with Ninja and builds. `--clean` forces a full rebuild.
- Needs **clang**, **ld.lld**, **NASM**, **CMake** and **Ninja** on PATH. No
  i686-elf GCC, no NMake, no MSVC Developer Command Prompt. The kernel is
  compiled with `clang --target=i686-elf`; override with `-DKCC=`,
  `-DCMAKE_LD=` or `-DCXK_TARGET=` if a host carries several LLVM versions.
- The image is `dist/CXK_x86_32/images/cxk_disk.img`. Run it with
  `cxk run dist/CXK_x86_32/images/cxk_disk.img` (QEMU), or `-e bochs`.
- Driving CMake directly still works: `cmake -S tools/cmake -B build -G Ninja
  -DCXK=<path to cxk> -DDEV_UNSIGNED=ON`, then `cmake --build build`.

## Test

- **Boot:** `ktest.c` runs at every boot and logs `self-tests: all N passed`.
  A kernel change is not done until a boot shows that.
- **Host suites** (Python until ported to `CXEX.Tests`, D1). They need the
  Release `cxk` at `devkit/CXEX.CLI/bin/Release/net10.0/cxk`, plus `gcc -m32`
  and `clang` on PATH (the X compiler assembles through `clang --target=i686-elf`):

  ```
  python3 devkit/tests/lang/run.py
  python3 devkit/tests/xdata/difftest.py
  python3 devkit/tests/xc/{lexdiff,parsediff,semadiff,asmdiff}.py
  python3 devkit/tests/xc/selfhost.py
  ```

  The last one must report three identical assemblies.
- **Checks:** `cxk check-abi` and `cxk check tools/cmake/CMakeLists.txt`.

## Where the plan lives

`docs/planning/HARDENING_PLAN.md` holds every standing decision (D1–D7) and the
phased checklist. Tick items there as they land. The order is: finish
Phase 0 (LLVM/Ninja build and `cxk os build`, `CXEX.Tests`), then Phase 1 (the
executable boundary in the CXEX loader), then roadmap stage 5 (an assembler and
linker in X). The reviews in `docs/reviews/` stay as written.
