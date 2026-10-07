# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
# Shared by the differential tests of the X compiler written in X (CXK os/xc)
# against the C# one: build one of its dump programs for the Linux host, make
# the corpus and its mutants, run both sides, and compare.
#
#   run(prog, verb, alphabet, window, edits)
#
# prog is the X program (tokdump, astdump), verb the `cxk` command printing
# the same format (tokens, ast). Needs: a Release build of CXEX.CLI, a CXK
# checkout (the platform root this devkit/ sits in, or CXK_ROOT) and a host gcc that can link
# -m32. The program is run natively - no CXK boot involved.
# Arguments: [mutants] [seed]. Exit 0 = every file agreed. SABOTAGE=1 corrupts
# one line of the X side's output, to show the test can fail.
import os, random, subprocess, sys, tempfile, shutil, glob

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
CXK = os.path.join(REPO, "CXEX.CLI", "bin", "Release", "net10.0", "cxk")
# Links the X compiler's output into a native 32-bit binary. clang, not gcc
# (HARDENING_PLAN D5): -nostdlib means no 32-bit libc is involved, so this needs
# no multilib. Override with CXOS_HOSTCC if a host only has gcc.
HOSTCC = os.environ.get("CXOS_HOSTCC", "clang")
ROOT = os.environ.get("CXK_ROOT") or os.path.normpath(os.path.join(REPO, ".."))

def find_os(prog):
    return next((d for d in (os.path.join(ROOT, "CXK", "os"), os.path.join(ROOT, "os"))
                 if os.path.exists(os.path.join(d, "xc", prog + ".xfxn"))), None)

# Building for the host: --no-prelude, the host platform first on the import
# path, and a start stub that hands Linux's argc/argv to main.
STUB = (".text\n.globl _start\n_start:\n    mov (%esp), %eax\n    lea 4(%esp), %ecx\n"
        "    push %ecx\n    push %eax\n    call main\n    mov %eax, %ebx\n    mov $1, %eax\n    int $0x80\n")

def build(osdir, prog, work):
    stub = os.path.join(work, "hstub.s")
    open(stub, "w").write(STUB)
    base = os.path.join(work, prog)
    r = subprocess.run([CXK, "compile", os.path.join(osdir, "xc", prog + ".xfxn"), base + ".elf", "--no-prelude",
                        "--emit-asm", "-I", os.path.join(osdir, "xc", "host"), "-I", os.path.join(osdir, "xc"),
                        "-I", os.path.join(osdir, "std")], capture_output=True, text=True, cwd=REPO)
    if not os.path.exists(base + ".s"): sys.exit(f"{prog} did not compile:\n" + r.stdout + r.stderr)
    s = open(base + ".s").read().replace(".globl _start", ".globl main", 1)
    open(base + ".s", "w").write(s)
    subprocess.run([HOSTCC, "-m32", "-nostdlib", "-static", "-o", base, base + ".s", stub], check=True)
    return base

def run(prog, verb, alphabet, window, edits, line_ops=False):
    if not os.path.exists(CXK): sys.exit(f"build the CLI first: dotnet build CXEX.CLI -c Release  ({CXK})")
    osdir = find_os(prog)
    if osdir is None: sys.exit(f"CXK checkout with os/xc/{prog}.xfxn not found: set CXK_ROOT")
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    work = tempfile.mkdtemp(prefix=prog + "-")
    try:
        exe = build(osdir, prog, work)

        # ---- the corpus: every real source, then mutants of windows of them ----
        real = sorted(set(glob.glob(os.path.join(osdir, "**", "*.xfxn"), recursive=True) +
                          glob.glob(os.path.join(REPO, "tests", "**", "*.xfxn"), recursive=True)))
        texts = [open(p, encoding="utf-8").read() for p in real]

        def mutate(t):
            if line_ops and rng.random() < 0.3:
                # whole lines dropped, doubled or moved - what breaks nesting
                ls = t.split("\n")
                for _ in range(rng.randint(1, 3)):
                    if not ls: break
                    i, j = rng.randrange(len(ls)), rng.randrange(len(ls))
                    op = rng.random()
                    if op < 0.4: del ls[i]
                    elif op < 0.7: ls.insert(j, ls[i])
                    else: ls[i], ls[j] = ls[j], ls[i]
                t = "\n".join(ls)
            t = list(t)
            for _ in range(rng.randint(1, edits)):
                p = rng.randint(0, len(t))
                op = rng.random()
                if op < 0.45: t[p:p] = list(rng.choice(alphabet))
                elif op < 0.75 and t: del t[min(p, len(t) - 1)]
                elif t: t[min(p, len(t) - 1)] = rng.choice(alphabet)
            return "".join(t)

        mdir = os.path.join(work, "m"); os.makedirs(mdir)
        mutants = []
        lo, hi = window
        for i in range(n):
            src = rng.choice(texts)
            a = rng.randint(0, max(0, len(src) - hi))
            doc = mutate(src[a:a + rng.randint(lo, hi)])
            p = os.path.join(mdir, f"m{i:05}.xfxn")
            open(p, "w", encoding="utf-8").write(doc)
            mutants.append(p)

        # ---- compare, 300 files a run ----
        fails = 0
        for label, files in (("real sources", real), ("mutants", mutants)):
            for k in range(0, len(files), 300):
                chunk = files[k:k + 300]
                c = subprocess.run([CXK, verb, *chunk], capture_output=True, cwd=REPO, timeout=600)
                x = subprocess.run([exe, *chunk], capture_output=True, timeout=600)
                if x.returncode != 0:
                    print(f"{prog} exited {x.returncode}: {x.stderr.decode(errors='replace')}")
                    fails += 1; break
                co, xo = c.stdout.decode("utf-8", "replace"), x.stdout.decode("utf-8", "replace")
                if os.environ.get("SABOTAGE") and label == "mutants" and k == 0:
                    xo = xo.replace("\n", "\nsabotage\n", 1)
                if co != xo:
                    cl, xl = co.split("\n"), xo.split("\n")
                    i = next(i for i in range(max(len(cl), len(xl)))
                             if i >= len(cl) or i >= len(xl) or cl[i] != xl[i])
                    hdr = next((l for l in reversed(cl[:i + 1]) if l.startswith("== ")), "?")
                    print(f"DIFFER in {hdr[3:]} at output line {i}:\n"
                          f"  C#: {cl[i] if i < len(cl) else '<end>'}\n  X:  {xl[i] if i < len(xl) else '<end>'}")
                    fails += 1
                    break
            print(f"{label}: {len(files)} files {'agree' if fails == 0 else 'DIFFER'}")
            if fails: break
        return 1 if fails else 0
    finally:
        if not os.environ.get("KEEP"): shutil.rmtree(work, ignore_errors=True)
        else: print("kept", work)
