# syntax=docker/dockerfile:1
#
# CXOS Platform CI image.
#
# GitHub Actions is not used; this image is the CI/CD pipeline's build
# environment, and it is deliberately runnable by hand so a local run and a
# pipeline run are the same run.
#
# It carries exactly the D5 toolchain - clang, ld.lld, NASM, CMake, Ninja - and
# no GCC: not even gcc-multilib, so an accidental dependency on a host GCC fails
# here instead of passing quietly. See HARDENING_PLAN.md D5.
#
#   docker build -f tools/ci/Dockerfile -t cxos-ci .
#   docker run --rm -v "$PWD:/src" -w /src cxos-ci tools/ci/build.sh
#
FROM mcr.microsoft.com/dotnet/sdk:10.0

# LLVM is PINNED, and from apt.llvm.org rather than the distro, because the
# version genuinely matters: for --target=i686-elf, clang **18 through 22 all
# ignore -fuse-ld=lld** and link via `gcc` instead, which fails outright on a
# host with no GCC. Caught here on 2026-10-07 at step 100 of 103 of the first
# Linux build, then measured version by version: 18, 19, 20, 21 and 22 fail; 23
# works. So 23 is a floor, not a preference - do not lower it to match a distro.
ARG LLVM_VERSION=23
RUN apt-get update \
 && apt-get install -y --no-install-recommends wget gnupg ca-certificates \
 && wget -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
 && echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      clang-${LLVM_VERSION} \
      lld-${LLVM_VERSION} \
      llvm-${LLVM_VERSION} \
 && for t in clang clang++ ld.lld lld llvm-objcopy llvm-objdump llvm-ar llvm-nm llvm-strip; do \
      ln -sf "/usr/lib/llvm-${LLVM_VERSION}/bin/$t" "/usr/local/bin/$t"; \
    done \
 && rm -rf /var/lib/apt/lists/*

# The rest of the toolchain, plus QEMU for boot verification (`cxk verify-boot`,
# the gate a kernel change has to pass) and xorriso/mtools for imaging. No
# Python: the last thing that needed it was the boot harness, which is now a cxk
# command. That also removed the only :latest pull (uv). The base image and the
# apt packages still float within their release lines; LLVM is the pinned one.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      nasm \
      cmake \
      ninja-build \
      qemu-system-x86 \
      xorriso \
      mtools \
      git \
 && rm -rf /var/lib/apt/lists/*

# Prove the thing that broke actually works, at image build time, so this image
# cannot ship with a clang that silently needs GCC to link. If this RUN fails,
# LLVM_VERSION is the problem - do not install gcc to make it pass.
RUN printf 'int main(void){return 0;}\n' > /tmp/smoke.c \
 && clang --target=i686-elf -ffreestanding -nostdlib -fuse-ld=lld -o /tmp/smoke.elf /tmp/smoke.c \
 && ! command -v gcc \
 && rm -f /tmp/smoke.c /tmp/smoke.elf \
 && echo 'smoke: clang links for i686-elf with no GCC present'

# .NET chatter that is noise in a CI log, and a NuGet cache inside the image.
ENV DOTNET_NOLOGO=1 \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
    NUGET_PACKAGES=/opt/nuget

# The kernel is built with clang --target=x86_64-elf; CMake is pointed at the same
# tools cxk pre-flights for, so driving CMake directly in here behaves.
ENV KCC=clang \
    CMAKE_LD=ld.lld \
    CXK_TARGET=x86_64-elf

WORKDIR /src
