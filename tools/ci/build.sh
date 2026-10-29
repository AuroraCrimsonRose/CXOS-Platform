#!/usr/bin/env bash
#
# The CI/CD build, start to finish. Runs inside tools/ci/Dockerfile:
#
#   docker build -f tools/ci/Dockerfile -t cxos-ci .
#   docker run --rm -v "$PWD:/src" -w /src cxos-ci tools/ci/build.sh
#
# It runs on a developer machine too, given the D5 toolchain on PATH. That is
# the point: one script, so a local run and a pipeline run are the same run.
#
# Stages are skippable so a pipeline can fan them out:
#   CXOS_CI_STAGES="devkit,os"   (default: devkit,os)
#   CXOS_CI_SUITES=1             also run the Differential suites (needs an ELF host)
#   CXOS_CI_CLI=<dir>            where cxk is published (default /tmp/cxos-ci/cli,
#                                outside the checkout; os needs devkit in the same run)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

STAGES="${CXOS_CI_STAGES:-devkit,os}"
has_stage() { [[ ",$STAGES," == *",$1,"* ]]; }
log() { printf '\n=== %s\n' "$*"; }

# --- pre-flight -------------------------------------------------------------
# D5's toolchain, and nothing outside it. Fail here with a readable message
# rather than 200 lines into CMake.
log 'pre-flight: toolchain'
missing=()
for t in dotnet clang ld.lld nasm cmake ninja; do
  command -v "$t" >/dev/null 2>&1 || missing+=("$t")
done
if (( ${#missing[@]} )); then
  printf 'missing from PATH: %s\n' "${missing[*]}" >&2
  exit 1
fi
printf 'clang   %s\n' "$(clang --version | head -1)"
printf 'ld.lld  %s\n' "$(ld.lld --version | head -1)"
printf 'nasm    %s\n' "$(nasm --version | head -1)"
printf 'cmake   %s\n' "$(cmake --version | head -1)"
printf 'ninja   %s\n' "$(ninja --version)"
printf 'dotnet  %s\n' "$(dotnet --version)"

# A host GCC must not be what makes this build work (D5). Say so out loud:
# the image has none, so this line is a canary for running somewhere that does.
if command -v gcc >/dev/null 2>&1; then
  printf 'note: gcc is on PATH (%s). The build must not need it.\n' "$(gcc -dumpversion 2>/dev/null || echo '?')"
fi

# --- devkit -----------------------------------------------------------------
# The cxk this run uses is the one it publishes, called where it was published.
# It used to be copied into tools/, which is the mounted checkout: a run without
# the devkit stage then picked up whatever an older run had left there, and the
# gitignored copy went stale (nine days, by 2026-10-16). It is published outside
# the checkout too, because `cxk os build --clean` empties build/, and a cxk
# published there deleted itself before CMake could run it.
CLI_DIR="${CXOS_CI_CLI:-/tmp/cxos-ci/cli}"
if has_stage devkit; then
  log 'devkit: build'
  dotnet build devkit/CXEX.slnx -c Release

  log 'devkit: test'
  dotnet test devkit/CXEX.slnx -c Release --no-build

  log 'devkit: publish cxk'
  dotnet publish devkit/CXEX.CLI -c Release -r linux-x64 \
    -p:SelfContained=true -p:PublishSingleFile=true \
    -p:IncludeNativeLibrariesForSelfExtract=true -p:DebugType=none \
    -o "$CLI_DIR"
fi

CXK="$CLI_DIR/cxk"
if [[ ! -x "$CXK" ]]; then
  printf 'error: %s is missing. Run the devkit stage first (CXOS_CI_STAGES=devkit,...).\n' "$CXK" >&2
  exit 1
fi

# --- registry and ABI checks ------------------------------------------------
# cxk os build runs these as pre-flights anyway; running them first means a
# version or ABI mismatch is reported as itself, not as a build failure.
log 'checks: versions, ABI, cmake'
"$CXK" check-versions
"$CXK" check-abi
"$CXK" check tools/cmake/CMakeLists.txt

# --- os ---------------------------------------------------------------------
if has_stage os; then
  log 'os: build (signed with an ephemeral key)'
  # The kernel's root of trust is generated from the public half of this same
  # key, so the key the kernel trusts is the key the build signs with. The
  # private half is destroyed below and never leaves the container.
  "$CXK" keygen tools/ci-ephemeral
  trap 'rm -f tools/ci-ephemeral.xksk tools/ci-ephemeral.xkpk' EXIT
  "$CXK" os build --key ci-ephemeral --clean
  rm -f tools/ci-ephemeral.xksk
  log 'os: image'
  ls -l dist/CXK_x86_64/images/cxk_x86_64.img

  # Boot what was just built. The kernel self-tests and the executive coming up
  # are the gate a kernel change has to pass (CLAUDE.md), and booting here is
  # what turns "Linux can build CXOS" into "Linux builds a CXOS that runs".
  # -snapshot inside verify-boot: the artifact is never modified by booting it.
  log 'os: boot (self-tests + executive)'
  "$CXK" verify-boot dist/CXK_x86_64/images/cxk_x86_64.img --full --timeout 300
fi

# --- differential suites ----------------------------------------------------
# Opt-in, because they are the slow ones: they compile X, link it into a
# *native* 32-bit binary and execute it, so they need an ELF host. On a host
# that is not Linux they skip, with the reason, rather than passing quietly.
# (These were nine Python scripts until D1 retired them on 2026-10-08.)
if [[ "${CXOS_CI_SUITES:-0}" == "1" ]]; then
  log 'tests: differential (X against C#)'
  dotnet test devkit/CXEX.Tests -c Release --filter "Category=Differential"
fi

log 'done'
