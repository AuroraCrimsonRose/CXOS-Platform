#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
# Differential test: the DevKit's X Data reader (CXEX.Lang/Data/XData.cs) against CXK's
# (os/std/xdata.xfxn). Every generated document goes through both; the error code AND the
# byte offset must match, or the build-time check and the supervisor disagree about a file.
#
#   python3 tests/xdata/difftest.py [count] [seed]
#
# Needs: a Release build of CXEX.CLI, a CXK checkout (the platform root this devkit/ sits in, or CXK_ROOT),
# and a host gcc that can link -m32 - the X side runs natively, not under QEMU.
# Exit 0 = every document agreed. Set SABOTAGE=1 to skew one expectation and watch it fail.
import os, random, subprocess, sys, collections, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
CXK = os.path.join(REPO, "CXEX.CLI", "bin", "Release", "net10.0", "cxk")
ROOT = os.environ.get("CXK_ROOT") or os.path.normpath(os.path.join(REPO, ".."))
STD = next((p for p in (os.path.join(ROOT, "CXK", "os", "std"), os.path.join(ROOT, "os", "std"))
            if os.path.exists(os.path.join(p, "xdata.xfxn"))), None)
if not os.path.exists(CXK): sys.exit(f"build the CLI first: dotnet build CXEX.CLI -c Release  ({CXK})")
if STD is None: sys.exit("CXK checkout not found: set CXK_ROOT")
SP = tempfile.mkdtemp(prefix="xdiff-")
# The emitted program's entry is renamed to main; this calls it and exits with its result.
open(os.path.join(SP, "stub.s"), "w").write(
    ".text\n.globl _start\n_start:\n    call main\n    mov %eax, %ebx\n    mov $1, %eax\n    int $0x80\n")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
rng = random.Random(SEED)

IDS = ["exec", "args", "start", "every", "grants", "a", "b_2", "x-y", "_k", "Service", "boot", "true", "false"]
def ident(): return rng.choice(IDS)
def ws(): return rng.choice(["", " ", "  ", "\t", " // c\n", " /* c */ ", "\n", "\r\n"])
def string():
    body = "".join(rng.choice(["a", " ", "\\n", "\\\"", "\\\\", "\\t", "\\0", "z", "/", "{", "}", "[", "#"]) for _ in range(rng.randint(0, 6)))
    return '"' + body + '"'
def integer():
    return rng.choice(["0", "7", "-12", "300", "0x1F", "0x0100_0000", "1_000", "4294967295", "99999999999", "-0"])
def value(d):
    r = rng.random()
    if d > 4 or r < 0.25: return rng.choice([string, integer, ident])()
    if r < 0.5:
        seps = [rng.choice([", ", ",\n", "\n", ","]) for _ in range(4)]
        items = [value(d + 1) for _ in range(rng.randint(0, 3))]
        out = "["
        for i, it in enumerate(items):
            out += ws() + it + (seps[i] if i < len(items) - 1 else rng.choice(["", ","]))
        return out + ws() + "]"
    tag = ident() + " " if rng.random() < 0.3 else ""
    return tag + "{" + entries(d + 1) + "}"
def entries(d):
    keys = rng.sample(IDS, rng.randint(0, 4))
    out = ""
    for i, k in enumerate(keys):
        out += ws() + k + rng.choice([" = ", "=", " =\n"]) + value(d)
        if i < len(keys) - 1: out += rng.choice([",", "\n", ", ", ",\n"])
    return out + ws()
ALPHA = list('{}[]",=/*\\\n\t -_0x9aZ') + ['//', '/*', '*/', '\\q', '\n\n']
def mutate(s):
    for _ in range(rng.randint(1, 3)):
        p = rng.randint(0, len(s))
        op = rng.random()
        if op < 0.4: s = s[:p] + rng.choice(ALPHA) + s[p:]
        elif op < 0.7 and s: s = s[:p] + s[p + 1:]
        elif s: s = s[:p] + rng.choice(ALPHA) + s[p + 1:]
    return s
def deep(n, kind):
    return "k = " + ("[" * n + "]" * n if kind else "{ k = " * n + "1" + " }" * n)

docs = []
for i in range(N):
    r = rng.random()
    if r < 0.02: docs.append(deep(rng.randint(28, 36), rng.random() < 0.5))
    elif r < 0.3: docs.append(entries(0))
    else: docs.append(mutate(entries(0)))
docs = [d.replace("\0", "") for d in docs]

# C# side
ddir = os.path.join(SP, "docs"); os.makedirs(ddir, exist_ok=True)
paths = []
for i, d in enumerate(docs):
    p = os.path.join(ddir, f"{i:05}.xd")
    with open(p, "wb") as f: f.write(d.encode("ascii"))
    paths.append(p)
cs = {}
for c in range(0, len(paths), 500):
    out = subprocess.run([CXK, "check-xdata", "--porcelain", *paths[c:c + 500]], capture_output=True, text=True).stdout
    for line in out.splitlines():
        f, code, at = line.split("\t"); cs[f] = (int(code), int(at))
exp = [cs[p] for p in paths]
if os.environ.get("SABOTAGE"):   # prove the harness can fail: skew one expected offset
    k = next(i for i, e in enumerate(exp) if e[0] != 0); exp[k] = (exp[k][0], exp[k][1] + 1)

# X side: batches of 200, each program returns the 1-based index of the first mismatch, or 0.
def xlit(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r") + '"'
fails = 0
for b in range(0, N, 200):
    chunk = list(range(b, min(b + 200, N)))
    src = ['import "xdata.xfxn";',
           'fn tlen(s: *u8) -> u32 { let n: u32 = 0; while (s[n] != 0) { n = n + 1; } return n; }',
           'fn t(d: *u8, code: i32, at: u32) -> bool { if (xd_check(d, tlen(d)) != code) { return false; } if (code == 0) { return true; } return xd_err_at == at; }',
           'fn main() -> i32 {']
    for j, i in enumerate(chunk):
        code, at = exp[i]
        src.append(f'    if (!t({xlit(docs[i])}, {code if code >= 0 else f"0 - {-code}"}, {at})) {{ return {j + 1}; }}')
    src += ['    return 0;', '}']
    xf = os.path.join(SP, f"batch{b // 200}.xfxn")
    open(xf, "w").write("\n".join(src) + "\n")
    base = xf[:-5]
    if os.path.exists(base + ".s"): os.remove(base + ".s")
    r = subprocess.run([CXK, "compile", xf, base + ".elf", "--no-prelude", "--emit-asm", "-I", STD], capture_output=True, text=True, cwd=REPO)
    # compile exits nonzero when clang is absent and it cannot assemble; only the
    # emitted .s matters here, which is linked natively below
    if "emitted" not in r.stdout: sys.exit("X compile failed: " + r.stdout + r.stderr)
    s = open(base + ".s").read().replace(".globl _start", ".globl main", 1)
    open(base + ".s", "w").write(s)
    subprocess.run(["gcc", "-m32", "-nostdlib", "-static", "-o", base + ".bin", base + ".s", os.path.join(SP, "stub.s")], check=True)
    rc = subprocess.run([base + ".bin"]).returncode
    if rc != 0:
        fails += 1
        i = chunk[rc - 1]
        print(f"MISMATCH batch {b // 200} doc {i}: C# says {exp[i]}; doc={docs[i]!r}")
print("codes:", dict(sorted(collections.Counter(c for c, _ in exp).items())))
print(f"{N} documents, seed {SEED}: {'all agree' if fails == 0 else f'{fails} batch(es) disagree'}")
import shutil; shutil.rmtree(SP, ignore_errors=True)
sys.exit(1 if fails else 0)
