#!/usr/bin/env python3
# Differential test: the X lexer written in X (CXK os/xc/lex.xfxn) against the
# C# one (CXEX.Lang/Lexer). Both print every token of the same files - kind,
# line:col, and value - and the outputs must be identical, byte for byte.
#
#   python3 tests/xc/lexdiff.py [mutants] [seed]
#
# The corpus is every .xfxn in CXK and in this repository, then `mutants`
# documents made by editing those at random: characters inserted, deleted and
# replaced from an alphabet that leans on what a lexer finds hard - quotes,
# backslashes, digits next to letters, `_`, comment openers, non-ASCII.
#
# Needs: a Release build of CXEX.CLI, a CXK checkout (CXK_ROOT, or ../CXK beside
# this repo), i686-elf-gcc on PATH and a host gcc that can link -m32. The X
# lexer is built for the Linux host with os/xc/host, and run natively.
# Exit 0 = every file agreed. SABOTAGE=1 corrupts one token of the X side's
# output, to show the test can fail.
import os, random, subprocess, sys, tempfile, shutil, glob
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
CXK = os.path.join(REPO, "CXEX.CLI", "bin", "Release", "net10.0", "cxk")
ROOT = os.environ.get("CXK_ROOT") or os.path.join(REPO, "..", "CXK")
OS = next((d for d in (os.path.join(ROOT, "CXK", "os"), os.path.join(ROOT, "os"))
           if os.path.exists(os.path.join(d, "xc", "lex.xfxn"))), None)
if not os.path.exists(CXK): sys.exit(f"build the CLI first: dotnet build CXEX.CLI -c Release  ({CXK})")
if OS is None: sys.exit("CXK checkout with os/xc/lex.xfxn not found: set CXK_ROOT")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
rng = random.Random(SEED)
W = tempfile.mkdtemp(prefix="lexdiff-")

# ---- build the X lexer for the host ----
stub = os.path.join(W, "hstub.s")
open(stub, "w").write(".text\n.globl _start\n_start:\n    mov (%esp), %eax\n    lea 4(%esp), %ecx\n"
                      "    push %ecx\n    push %eax\n    call main\n    mov %eax, %ebx\n    mov $1, %eax\n    int $0x80\n")
base = os.path.join(W, "tokdump")
r = subprocess.run([CXK, "compile", os.path.join(OS, "xc", "tokdump.xfxn"), base + ".elf", "--no-prelude", "--emit-asm",
                    "-I", os.path.join(OS, "xc", "host"), "-I", os.path.join(OS, "xc"), "-I", os.path.join(OS, "std")],
                   capture_output=True, text=True, cwd=REPO)
if not os.path.exists(base + ".s"): sys.exit("tokdump did not compile:\n" + r.stdout + r.stderr)
s = open(base + ".s").read().replace(".globl _start", ".globl main", 1)
open(base + ".s", "w").write(s)
subprocess.run(["gcc", "-m32", "-nostdlib", "-static", "-o", base, base + ".s", stub], check=True)

# ---- the corpus ----
real = sorted(set(glob.glob(os.path.join(OS, "**", "*.xfxn"), recursive=True) +
                  glob.glob(os.path.join(REPO, "tests", "**", "*.xfxn"), recursive=True)))
texts = [open(p, encoding="utf-8").read() for p in real]
ALPHA = list("\"'\\_0123456789xXuUabcdefz{}[]()<>=!&|^~+-*/%.,;:@ \t\n") + \
        ["//", "/*", "*/", "0x", "u8", "i128", "\\x", "\\n", "é", "😀", "\r\n", "1_000", "'a'", "\"s\""]
def mutate(t):
    t = list(t)
    for _ in range(rng.randint(1, 6)):
        p = rng.randint(0, len(t))
        op = rng.random()
        if op < 0.45: t[p:p] = list(rng.choice(ALPHA))
        elif op < 0.75 and t: del t[min(p, len(t) - 1)]
        elif t: t[min(p, len(t) - 1)] = rng.choice(ALPHA)
    return "".join(t)
mdir = os.path.join(W, "m"); os.makedirs(mdir)
mutants = []
for i in range(N):
    src = rng.choice(texts)
    # a window, so a mutant is small enough to land its edits somewhere interesting
    a = rng.randint(0, max(0, len(src) - 400)); doc = mutate(src[a:a + rng.randint(20, 400)])
    p = os.path.join(mdir, f"m{i:05}.xfxn")
    open(p, "w", encoding="utf-8").write(doc)
    mutants.append(p)

# ---- compare ----
def run_both(files):
    c = subprocess.run([CXK, "tokens", *files], capture_output=True, text=True, cwd=REPO).stdout
    x = subprocess.run([base, *files], capture_output=True).stdout.decode("utf-8", "replace")
    return c, x

fails = 0
for label, files in (("real sources", real), ("mutants", mutants)):
    for k in range(0, len(files), 300):
        chunk = files[k:k + 300]
        c, x = run_both(chunk)
        if os.environ.get("SABOTAGE") and label == "mutants" and k == 0:
            x = x.replace("Identifier", "Identifiex", 1)
        if c != x:
            cl, xl = c.split("\n"), x.split("\n")
            i = next(i for i in range(max(len(cl), len(xl))) if i >= len(cl) or i >= len(xl) or cl[i] != xl[i])
            hdr = next((l for l in reversed(cl[:i + 1]) if l.startswith("== ")), "?")
            print(f"DIFFER in {hdr[3:]} at output line {i}:\n  C#: {cl[i] if i < len(cl) else '<end>'}\n  X:  {xl[i] if i < len(xl) else '<end>'}")
            fails += 1
            break
    print(f"{label}: {len(files)} files {'agree' if fails == 0 else 'DIFFER'}")
    if fails: break

shutil.rmtree(W, ignore_errors=True)
sys.exit(1 if fails else 0)
