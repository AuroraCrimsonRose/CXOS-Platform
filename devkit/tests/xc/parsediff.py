#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
# Differential test: the X parser written in X (CXK os/xc/parse.xfxn) against
# the C# one (CXEX.Lang/Parsing). Both print the tree they build for the same
# files - every node, where it starts, what it holds - and then every syntax
# error with its line, column and message. The outputs must be identical.
#
#   python3 tests/xc/parsediff.py [mutants] [seed]
#
# The corpus is every .xfxn in CXK and in this repository, then `mutants`
# documents cut from those and broken at random: whole tokens inserted,
# deleted and replaced - brackets, keywords, operators - and whole lines
# dropped, doubled and swapped, which is what tests error recovery: where a
# parser resynchronises decides everything it says after the first mistake.
# Requirements and SABOTAGE=1: see xcdiff.py.
import sys, xcdiff

ALPHA = list("{}[]()<>=!&|^~+-*/%.,;:@ \n") + [
    "fn ", "struct ", "enum ", "global ", "const ", "extern ", "type ", "import ", "let ", "if ", "else ",
    "while ", "return ", "break;", "continue;", "defer ", "switch ", "case ", "as ", "sizeof ", "true", "false",
    "->", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "+=", "-=", "<<=", "|=", "x", "y.z", "f(a)", "e.v(n)",
    "pt { x: 1 }", "[1, 2]", "*user u8", "[4]u32", "fn(u32) -> bool", "u128", "1u64", "'c'", "\"s\"",
    "@attr", "@a(k = 1)", "{ }", "( )", "a = b;", "x: i32", "4294967296", "é",
]

sys.exit(xcdiff.run("astdump", "ast", ALPHA, (40, 3000), 8, line_ops=True))
