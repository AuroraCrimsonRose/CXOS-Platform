#!/usr/bin/env python3
# X language tests, run natively on the host.
#
#   python3 tests/lang/run.py
#
#   run/      each program must compile and exit 0. A nonzero exit is the
#             number of the first check that failed - read the source.
#   refuse/   each program must FAIL to compile, with the message in its
#             EXPECT table below. A program that compiles is a wrong answer
#             the compiler has stopped catching.
#   interop/  x.xfxn compiled with --object and linked with c.c: C calls X and
#             X calls C, with 64-bit values and structs by value.
#
# Needs: a Release build of CXEX.CLI, i686-elf-gcc on PATH (the compiler
# assembles with it) and a host gcc that can link -m32. Exit 0 = all passed.
import os, subprocess, sys, tempfile, shutil, glob
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
CXK = os.path.join(REPO, "CXEX.CLI", "bin", "Release", "net10.0", "cxk")
if not os.path.exists(CXK): sys.exit(f"build the CLI first: dotnet build CXEX.CLI -c Release  ({CXK})")
W = tempfile.mkdtemp(prefix="xlang-")
STUB = os.path.join(W, "stub.s")
open(STUB, "w").write(".text\n.globl _start\n_start:\n    call main\n    mov %eax, %ebx\n    mov $1, %eax\n    int $0x80\n")

EXPECT = {
    "narrow.xfxn":       ["narrowing needs a cast: `as u32`"],
    "shift_width.xfxn":  ["shift of 100 is outside 0..31", "shift of 40 is outside 0..31"],
    "suffix_range.xfxn": ["256 does not fit in u8", "128 does not fit in i8", "129 does not fit in i8"],
    "array_param.xfxn":  ["is an array; pass a pointer"],
}

def compile_x(src, out, *extra):
    return subprocess.run([CXK, "compile", src, out, "--no-prelude", *extra],
                          capture_output=True, text=True, cwd=REPO)

fails = 0
for src in sorted(glob.glob(os.path.join(HERE, "run", "*.xfxn"))):
    name = os.path.basename(src); base = os.path.join(W, name[:-5])
    r = compile_x(src, base + ".elf", "--emit-asm")
    if not os.path.exists(base + ".s"):
        print(f"FAIL {name}: did not compile\n{r.stdout}{r.stderr}"); fails += 1; continue
    s = open(base + ".s").read().replace(".globl _start", ".globl main", 1)
    open(base + ".s", "w").write(s)
    subprocess.run(["gcc", "-m32", "-nostdlib", "-static", "-o", base + ".bin", base + ".s", STUB],
                   check=True, capture_output=True)
    rc = subprocess.run([base + ".bin"]).returncode
    print(f"{'ok  ' if rc == 0 else 'FAIL'} {name}" + ("" if rc == 0 else f": check {rc} failed"))
    fails += rc != 0

for src in sorted(glob.glob(os.path.join(HERE, "refuse", "*.xfxn"))):
    name = os.path.basename(src)
    r = compile_x(src, os.path.join(W, name[:-5] + ".elf"))
    out = r.stdout + r.stderr
    missing = [m for m in EXPECT.get(name, []) if m not in out]
    ok = "compilation failed" in out and not missing
    print(f"{'ok  ' if ok else 'FAIL'} refuse/{name}" + ("" if ok else f": missing {missing}"))
    fails += not ok

xo = os.path.join(W, "x.o")
r = compile_x(os.path.join(HERE, "interop", "x.xfxn"), xo, "--object")
if not os.path.exists(xo):
    print(f"FAIL interop: X did not compile\n{r.stdout}{r.stderr}"); fails += 1
else:
    exe = os.path.join(W, "interop.bin")
    subprocess.run(["gcc", "-m32", "-O2", "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-stack-protector",
                    "-nostdlib", "-static", "-o", exe, os.path.join(HERE, "interop", "c.c"), xo, STUB],
                   check=True, capture_output=True)
    rc = subprocess.run([exe]).returncode
    print(f"{'ok  ' if rc == 0 else 'FAIL'} interop (C <-> X)" + ("" if rc == 0 else f": check {rc} failed"))
    fails += rc != 0

shutil.rmtree(W, ignore_errors=True)
print("all passed" if fails == 0 else f"{fails} failed")
sys.exit(1 if fails else 0)
