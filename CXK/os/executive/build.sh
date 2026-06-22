#!/bin/sh
# /CXK/os/executive/build.sh
# Build the system executive into a signed .xoex:
#   executive.c --(cross gcc + executive.ld)--> ELF
#               --(mkcxes --type os)----------> .xoex
#               --(signcxex, kernel key)------> signed .xoex
# Uses the i686-elf cross toolchain (override CC if yours differs).
set -e
CC=${CC:-i686-elf-gcc}
HERE=$(dirname "$0")
TOOLS="$HERE/../../tools/CXEX_Compiler"
OUT=${1:-executive.xoex}

"$CC" -ffreestanding -fno-pic -no-pie -fno-stack-protector -nostdlib \
      -Wl,-T,"$HERE/executive.ld" -o "$HERE/executive.elf" "$HERE/executive.c"

python3 "$TOOLS/mkcxes.py"   "$HERE/executive.elf" "$OUT" --type os
python3 "$TOOLS/signcxex.py" "$OUT" "$TOOLS/kernel.xksk" "$TOOLS/kernel.xkpk"
echo "built signed executive: $OUT"