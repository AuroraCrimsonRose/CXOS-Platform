# CXOS Platform — working notes

One repository for CXOS: **CXK** (the kernel), the **X userland** (including
`os/xc`, the X compiler written in X), the boot chain, and the **CX DevKit**
(`devkit/`: the C# X compiler, the `cxk` CLI, signing, imaging, CXEX Studio).
The owner holds the only copy, so backwards compatibility is never a
constraint. Change formats and interfaces freely, and update every user in the
same commit.

## Branches

- **`x86_32_DEV`**: **master** — active development, and the branch everything
  else is measured against. Commit and push here directly. Do **not** create
  per-session or feature branches unless asked.
- **`x86_32_RELEASE`**: full releases only, merged from `x86_32_DEV` by PR when
  asked. Never push to it directly.
- A `v*` tag runs `.github/workflows/release.yml` and publishes the assets in
  `docs/planning/VERSIONING_AND_RELEASE.md` §5. `vsix-v*` is the VS Code
  extension's own line.
- Open a PR only when asked.
- Stale, do not use: `x32_86` and `x32_86_DEV` are a typo'd pair, and
  `origin/HEAD` still points at `x32_86`.

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
  `os/executive/app_image.h`, `bin/`,
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
-p:SelfContained=true -p:PublishSingleFile=true`.

- **Every host:** `cxk os build` (signed if `tools/kernel.xksk` exists) or
  `cxk os build --dev` (development kernel: runs unsigned programs, never ship
  it). It pre-flights the toolchain, the source list and the ABI, then
  configures CMake with Ninja and builds. `--clean` forces a full rebuild.
- **Keys:** `--key <name>` selects the pair `tools/<name>.xksk` +
  `tools/<name>.xkpk`; the default is `kernel`. The kernel's root of trust
  (`trusted_key.c`) is **generated into `build/` from the public half of that
  same key** on every build, so the key the kernel trusts is always the key the
  build signs with — embed one and sign with the other and every artifact is
  refused at boot as `BAD_SIGNATURE`, which reads as tampering rather than as
  the wrong key. `cxk keygen tools/test` then `cxk os build --key test` is the
  way to exercise the **signed** path locally; the pair stays out of git.
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
- **Checks:** `cxk check-abi`, `cxk check-versions` and
  `cxk check tools/cmake/CMakeLists.txt`. `cxk os build` runs all three as
  pre-flights.

## Versions

`versions.json` at the repository root **decides every version** — the five
shipping components and the eight formats. Nothing else is authoritative: each
entry names the files that must agree with it, and `cxk check-versions` enforces
them. If a number disagrees with the registry, the number is wrong.

- Components (CXOS, CXK, `cxk`, Studio, the VSIX) each carry **their own semver**
  and move at their own pace. All five are at 1.0.0.
- Formats (CXEX, ABI, CXBI, CXFS, XBPT, XSTG, XKPK) are **monotonic integers**,
  never reset, bumped in the commit that changes the format on every side at once.
- To add a place a version is written, add a `checks` entry — never edit the
  checker.

Each major **CXOS** release also carries a Greek mythological name — **1.x is
"Hekate"**. It names the **OS release only**: never the kernel, the boot chain or
the tooling, which have versions and no names. A new major's name is recommended
from what that generation means and is **not official until Aurora approves it**.

`docs/planning/VERSIONING_AND_RELEASE.md` has what a bump means, how a release is
cut, what the workflow produces, and how signing keys are selected.

## Tooling, and keeping context cheap

This is a large repository and most sessions end by running out of context, not
by running out of work. Use the MCP servers, and treat every tool result as
something you pay for twice — once to read it, and again in every later turn it
sits in the window.

### The servers

- **`codebase-memory`** (HTTP, `http://localhost:8092/mcp`, project
  `E-CXOS-Platform`) — a symbol graph over the whole tree. `search_graph` to find
  a symbol, `trace_path` for how two are related, `get_code_snippet` to pull
  **one function instead of a 1000-line file**, `get_file_outline` for a file's
  shape, `query_graph` / `get_architecture` for structure, `search_code` for
  literals. Two things to know: `search_code`'s parameter is **`pattern`**, not
  `query`, and `check_index_coverage` is what tells you whether a file you are
  about to rely on is actually indexed — if it is not, read the reported lines
  and say your conclusion is qualified. A hook also injects graph hits into
  `Bash` results, so grepping for a symbol name gets graph context for free.
- **`repomix`** — `pack_codebase` on **one subdirectory** with compression, then
  `grep_repomix_output` / `read_repomix_output`. For "what is the surface of this
  subsystem" questions. Packing the repository root is never the right call.
- **`desktop-commander`** — `edit_block` for text edits; use it instead of
  `sed`/`perl`/`awk` for anything containing backslashes or regex
  metacharacters. This repository has already been damaged twice that way: a
  `sed` wrote `\b` into `README.md` as a literal backspace byte, and `perl`
  substitutions collided with `$1` and with `|` as a delimiter in
  `CMakeLists.txt` and `spawn.c`. Also `start_process`/`interact_with_process`
  for a long-lived REPL, and `start_search` for large searches.
- **`delegate-local`** — a local Qwen3.6-35B (~100k context) behind LiteLLM at
  `http://localhost:8080`, model `openai-qwen-35b`, dispatched with
  `delegate_to_local_agent` and the `local-worker` agent (it has the
  codebase-memory tools too). Give it bulk reading, summarising, boilerplate,
  mechanical edits and classification sweeps — work whose output you can check
  faster than you could produce it. **Always check it.** It has produced a
  "known-good" CXEX test baseline that was W+X (`flags = 0x6`, the exact thing
  the loader must refuse) and mis-classified a writing syscall as read-only.
  On Windows it needs `DELEGATE_ENV_PASSTHROUGH` to include `SYSTEMROOT` — its
  env allowlist is POSIX-only, and without it curl exits 7 and PowerShell throws
  `0x8009001d` — and `DELEGATE_KEEP_TOOL_RESULTS` is raised from 6 to 16.

### What actually saves context, in order of how much

1. **Never read a whole file to find one thing.** `grep -n` with `-A`/`-B`,
   `sed -n 'a,bp'`, or `get_code_snippet`.
2. **Filter every command.** `| tail -30`, `| grep -iE '\berror\b|warning:'`,
   `| sort -u`. An OS build prints ~300 lines of which four matter; a QEMU
   `-d int` log is tens of thousands of lines whose whole content is
   `grep -o 'v=[0-9a-f][0-9a-f]' | sort -u`.
3. **Count before you list.** `grep -c`, `wc -l`, `git diff --stat` before
   `git diff`, `git status --short`.
4. **One call, several commands**, joined with `;` and `echo ===` separators,
   whenever they are independent. Parallel tool calls in one block for the rest.
5. **Keep intermediates out of the transcript.** Capture dumps, decoders and
   boot-verification loops belong in a scratchpad script that prints only the
   line you are looking for. Write the script once and re-run it.
6. **Delegate bulk reading, never judgement.** The sweep goes to Qwen; the call
   about whether a check is correct does not.

### Two traps that have cost real work here

- `git checkout <file>` to undo a **sabotage edit** also throws away any
  uncommitted work in that file. Sabotage-test from a committed base, or revert
  the edit the same way you made it.
- A test that passes tells you nothing until you have seen it fail. Every
  verification in this repository is paired: delete the check, rebuild, and
  require exactly the tests that cover it to go red. Several "passing" tests
  here were passing for the wrong reason — a mutation that was a no-op, a
  builder that sized a file around the field under test, a disk test silently
  skipped on the wrong machine type.

## Where the plan lives

`docs/planning/HARDENING_PLAN.md` holds every standing decision (D1–D7) and the
phased checklist. Tick items there as they land. The order is: finish
Phase 0 (LLVM/Ninja build and `cxk os build`, `CXEX.Tests`), then Phase 1 (the
executable boundary in the CXEX loader), then roadmap stage 5 (an assembler and
linker in X). The reviews in `docs/reviews/` stay as written.
