#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
# The X compiler compiling itself.
#
#   uv run --python 3.12 tests/xc/selfhost.py
#
# The classic bootstrap check, on CXK os/xc/xc.xfxn:
#   stage 1  the C# compiler compiles xc            -> xc1
#   stage 2  xc1 compiles xc                        -> xc2
#   stage 3  xc2 compiles xc                        -> its assembly
# The assembly of all three must be identical - stage 2's is what the C#
# compiler wrote, and stage 3's is what a compiler that X built writes. Then
# xc2, the compiler compiled by X, compiles every program in tests/lang/run,
# and each must run and exit 0, as they do compiled by the C# one.
#
# Needs what xcdiff.py needs. Exit 0 = a fixed point, and working programs.
import os, subprocess, sys, tempfile, shutil, glob
import xcdiff

if not os.path.exists(xcdiff.CXK): sys.exit(f"build the CLI first ({xcdiff.CXK})")
OS = xcdiff.find_os("xc")
if OS is None: sys.exit("CXK checkout with os/xc/xc.xfxn not found: set CXK_ROOT")
W = tempfile.mkdtemp(prefix="selfhost-")
INC = ["-I", os.path.join(OS, "xc", "host"), "-I", os.path.join(OS, "xc"), "-I", os.path.join(OS, "std")]
SRC = os.path.join(OS, "xc", "xc.xfxn")


def link(asm, exe):
    """A host binary from a compiler's assembly: main for _start, and the stub."""
    s = open(asm).read().replace(".globl _start", ".globl main", 1)
    fixed = exe + ".host.s"
    open(fixed, "w").write(s)
    stub = os.path.join(W, "hstub.s")
    if not os.path.exists(stub): open(stub, "w").write(xcdiff.STUB)
    subprocess.run([xcdiff.HOSTCC, "-m32", "-nostdlib", "-static", "-o", exe, fixed, stub], check=True)


def xc(compiler, out, *args):
    r = subprocess.run([compiler, "--no-prelude", *INC, *args, "-o", out], capture_output=True, text=True)
    if r.returncode != 0: sys.exit(f"{compiler} failed:\n{r.stdout}{r.stderr}")


try:
    # stage 1: C#
    s1 = os.path.join(W, "xc1.s")
    r = subprocess.run([xcdiff.CXK, "compile", SRC, os.path.join(W, "xc1.elf"), "--no-prelude", "--emit-asm", *INC],
                       capture_output=True, text=True, cwd=xcdiff.REPO)
    if not os.path.exists(s1): sys.exit("stage 1 did not compile:\n" + r.stdout + r.stderr)
    link(s1, os.path.join(W, "xc1"))

    # stage 2: xc1 compiles xc
    s2 = os.path.join(W, "xc2.s")
    xc(os.path.join(W, "xc1"), s2, SRC)
    link(s2, os.path.join(W, "xc2"))

    # stage 3: xc2 compiles xc
    s3 = os.path.join(W, "xc3.s")
    xc(os.path.join(W, "xc2"), s3, SRC)

    a1, a2, a3 = (open(p).read() for p in (s1, s2, s3))
    print(f"stage 1 (C#):          {len(a1.splitlines())} lines")
    print(f"stage 2 (xc by C#):    {'identical' if a2 == a1 else 'DIFFERENT'}")
    print(f"stage 3 (xc by xc):    {'identical' if a3 == a2 else 'DIFFERENT'}")
    fails = (a2 != a1) + (a3 != a2)

    # what the self-compiled compiler builds must work
    run_stub = os.path.join(W, "stub.s")
    open(run_stub, "w").write(".text\n.globl _start\n_start:\n    call main\n    mov %eax, %ebx\n    mov $1, %eax\n    int $0x80\n")
    progs = sorted(glob.glob(os.path.join(xcdiff.REPO, "tests", "lang", "run", "*.xfxn")))
    ok = 0
    for p in progs:
        base = os.path.join(W, os.path.basename(p)[:-5])
        r = subprocess.run([os.path.join(W, "xc2"), "--no-prelude", p, "-o", base + ".s"], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"FAIL {os.path.basename(p)}: xc2 did not compile it\n{r.stdout}{r.stderr}"); fails += 1; continue
        s = open(base + ".s").read().replace(".globl _start", ".globl main", 1)
        open(base + ".s", "w").write(s)
        subprocess.run([xcdiff.HOSTCC, "-m32", "-nostdlib", "-static", "-o", base, base + ".s", run_stub], check=True)
        rc = subprocess.run([base]).returncode
        if rc != 0: print(f"FAIL {os.path.basename(p)}: exit {rc}"); fails += 1
        else: ok += 1
    print(f"tests/lang/run compiled by xc2: {ok} of {len(progs)} run correctly")
    sys.exit(1 if fails else 0)
finally:
    if not os.environ.get("KEEP"): shutil.rmtree(W, ignore_errors=True)
