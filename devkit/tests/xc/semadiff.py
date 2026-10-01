#!/usr/bin/env python3
# Differential test: the X type checker written in X (CXK os/xc/sema.xfxn) against
# the C# one (CXEX.Lang/Sema). Both analyse the same whole programs - the
# prelude, the file, everything it imports - and print every diagnostic, then
# the type of every expression and local. The outputs must be identical.
#
#   python3 tests/xc/semadiff.py [mutants] [seed]
#
# The corpus is every X file in CXK (each app and std module with the prelude,
# each piece of the X compiler without it, as their builds do) and every
# program in tests/lang. Mutants change a file a few tokens at a time - a name
# for another name or a type, a literal for one at a type's edge, an operator
# for another, a cast or a unary operator added, a line dropped or doubled -
# so most still parse, and go on to the resolver and the type checker.
# Requirements and SABOTAGE=1: see xcdiff.py.
import os, random, re, subprocess, sys, tempfile, shutil, glob
import xcdiff

if not os.path.exists(xcdiff.CXK): sys.exit(f"build the CLI first ({xcdiff.CXK})")
OS = xcdiff.find_os("semadump")
if OS is None: sys.exit("CXK checkout with os/xc/semadump.xfxn not found: set CXK_ROOT")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
W = tempfile.mkdtemp(prefix="semadiff-")

try:
    exe = xcdiff.build(OS, "semadump", W)
    prelude = os.path.join(W, "prelude.xfxn")
    with open(prelude, "wb") as f:
        f.write(subprocess.run([xcdiff.CXK, "prelude"], capture_output=True, check=True).stdout)

    STD = ["-I", os.path.join(OS, "std")]
    XC = ["-I", os.path.join(OS, "xc", "host"), "-I", os.path.join(OS, "xc"), "-I", os.path.join(OS, "std")]
    # (file, include dirs, with the prelude?)
    corpus = []
    for p in sorted(glob.glob(os.path.join(OS, "apps", "*.xfxn")) + glob.glob(os.path.join(OS, "std", "*.xfxn"))):
        corpus.append((p, STD, True))
    for p in sorted(glob.glob(os.path.join(OS, "xc", "*.xfxn"))):
        corpus.append((p, XC, False))
    for p in sorted(glob.glob(os.path.join(xcdiff.REPO, "tests", "lang", "*", "*.xfxn"))):
        corpus.append((p, [], False))

    # ---- mutants ----
    TOK = re.compile(r'//[^\n]*|/\*.*?\*/|\s+|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|'
                     r'0[xX][0-9A-Fa-f_]+\w*|\d[\d_]*\w*|[A-Za-z_]\w*|<<=|>>=|->|==|!=|<=|>=|&&|\|\||<<|>>|[-+*/%&|^]=|.', re.S)
    TYPES = ["u8", "u16", "u32", "u64", "u128", "i8", "i16", "i32", "i64", "i128", "bool", "void"]
    NUMS = ["0", "1", "2", "7", "31", "32", "63", "64", "127", "128", "255", "256", "65535", "0x7FFFFFFF", "0x80000000",
            "0xFFFFFFFF", "4294967296", "1u8", "300u8", "1u64", "128i8", "1i128", "'a'", "'\\n'", "true", "false"]
    OPS = ["+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==", "!=", "<", ">", "<=", ">=", "&&", "||"]
    CASTS = [" as u8", " as u32", " as i32", " as u64", " as u128", " as i8", " as *u8", " as bool", " as *phys u8"]
    UNARY = ["&", "*", "-", "!", "~"]

    def mutate(text):
        toks = TOK.findall(text)
        code = [i for i, t in enumerate(toks) if not (t.isspace() or t.startswith("//") or t.startswith("/*"))]
        idents = [toks[i] for i in code if re.fullmatch(r"[A-Za-z_]\w*", toks[i])]
        for _ in range(rng.randint(1, 3)):
            i = rng.choice(code)
            t = toks[i]
            op = rng.random()
            if re.fullmatch(r"[A-Za-z_]\w*", t):
                if op < 0.55: toks[i] = rng.choice(idents)
                elif op < 0.7: toks[i] = rng.choice(TYPES)
                elif op < 0.85: toks[i] = t + rng.choice(CASTS)
                else: toks[i] = rng.choice(UNARY) + t
            elif re.fullmatch(r"\d\w*|0[xX]\w+|'.*'", t):
                toks[i] = rng.choice(NUMS)
            elif t in OPS:
                toks[i] = rng.choice(OPS)
            elif t == ")" and op < 0.3:
                toks[i] = ")" + rng.choice(CASTS)
            elif op < 0.1:
                toks[i] = ""
        out = "".join(toks)
        if rng.random() < 0.15:   # a line doubled or dropped: declarations twice, or missing
            ls = out.split("\n")
            j = rng.randrange(len(ls))
            if rng.random() < 0.5: ls.insert(j, ls[j])
            else: del ls[j]
            out = "\n".join(ls)
        return out

    mutants = []
    for k in range(N):
        src, incs, pre = rng.choice(corpus)
        d = os.path.join(W, "m", f"{k:05}")
        os.makedirs(d)
        p = os.path.join(d, os.path.basename(src))
        open(p, "w", encoding="utf-8").write(mutate(open(src, encoding="utf-8").read()))
        # the original's directory stays on the path, for what it imports beside itself
        mutants.append((p, incs + ["-I", os.path.dirname(src)], pre))

    # ---- compare: programs sharing their options go together, 100 a run ----
    def run_both(group, incs, pre):
        c = subprocess.run([xcdiff.CXK, "sema", *([] if pre else ["--no-prelude"]), *incs, *group],
                           capture_output=True, cwd=xcdiff.REPO, timeout=1200)
        x = subprocess.run([exe, *(["--prelude", prelude] if pre else ["--no-prelude"]), *incs, *group],
                           capture_output=True, timeout=1200)
        if x.returncode != 0:
            return None, f"semadump exited {x.returncode}: {x.stderr.decode(errors='replace')}"
        return c.stdout.decode("utf-8", "replace"), x.stdout.decode("utf-8", "replace")

    fails = 0
    stats = {"programs": 0, "checked": 0, "errors": 0}
    for label, items in (("real sources", [(p, i, pre) for p, i, pre in corpus]), ("mutants", mutants)):
        groups = {}
        for p, incs, pre in items: groups.setdefault((tuple(incs), pre), []).append(p)
        first = True
        for (incs, pre), files in groups.items():
            for k in range(0, len(files), 100):
                chunk = files[k:k + 100]
                co, xo = run_both(chunk, list(incs), pre)
                if co is None:
                    print(xo); fails += 1; break
                if os.environ.get("SABOTAGE") and label == "mutants" and first:
                    xo = xo.replace("\n", "\nsabotage\n", 1)
                first = False
                stats["programs"] += co.count("\n== ") + co.startswith("== ")
                stats["checked"] += co.count("checked yes")
                stats["errors"] += co.count("\nerror ")
                if co != xo:
                    cl, xl = co.split("\n"), xo.split("\n")
                    i = next(i for i in range(max(len(cl), len(xl))) if i >= len(cl) or i >= len(xl) or cl[i] != xl[i])
                    hdr = next((l for l in reversed(cl[:i + 1]) if l.startswith("== ")), "?")
                    print(f"DIFFER in {hdr[3:]} at output line {i}:\n"
                          f"  C#: {cl[i] if i < len(cl) else '<end>'}\n  X:  {xl[i] if i < len(xl) else '<end>'}")
                    fails += 1
                    break
            if fails: break
        print(f"{label}: {len(items)} programs {'agree' if fails == 0 else 'DIFFER'}")
        if fails: break
    print(f"({stats['checked']} of {stats['programs']} reached the type checker; {stats['errors']} diagnostics)")
    sys.exit(1 if fails else 0)
finally:
    if not os.environ.get("KEEP"): shutil.rmtree(W, ignore_errors=True)
    else: print("kept", W)
