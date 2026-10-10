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
#   CXOS_CI_SUITES=1             also run the legacy Python suites (needs an ELF host)
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
if has_stage devkit; then
  log 'devkit: build'
  dotnet build devkit/CXEX.Studio.slnx -c Release

  log 'devkit: test'
  dotnet test devkit/CXEX.Studio.slnx -c Release --no-build

  log 'devkit: publish cxk into tools/'
  dotnet publish devkit/CXEX.CLI -c Release -r linux-x64 \
    -p:SelfContained=true -p:PublishSingleFile=true \
    -p:IncludeNativeLibrariesForSelfExtract=true -p:DebugType=none \
    -o build/ci/cli
  install -m 0755 build/ci/cli/cxk tools/cxk
fi

# --- registry and ABI checks ------------------------------------------------
# cxk os build runs these as pre-flights anyway; running them first means a
# version or ABI mismatch is reported as itself, not as a build failure.
log 'checks: versions, ABI, cmake'
tools/cxk check-versions
tools/cxk check-abi
tools/cxk check tools/cmake/CMakeLists.txt

# --- os ---------------------------------------------------------------------
if has_stage os; then
  log 'os: build (signed with an ephemeral key)'
  # The kernel's root of trust is generated from the public half of this same
  # key, so the key the kernel trusts is the key the build signs with. The
  # private half is destroyed below and never leaves the container.
  tools/cxk keygen tools/ci-ephemeral
  trap 'rm -f tools/ci-ephemeral.xksk tools/ci-ephemeral.xkpk' EXIT
  tools/cxk os build --key ci-ephemeral --clean
  rm -f tools/ci-ephemeral.xksk
  log 'os: image'
  ls -l dist/CXK_x86_32/images/cxk_disk.img
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
