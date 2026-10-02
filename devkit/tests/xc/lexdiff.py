#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
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
# Requirements and SABOTAGE=1: see xcdiff.py.
import sys, xcdiff

ALPHA = list("\"'\\_0123456789xXuUabcdefz{}[]()<>=!&|^~+-*/%.,;:@ \t\n") + \
        ["//", "/*", "*/", "0x", "u8", "i128", "\\x", "\\n", "é", "😀", "\r\n", "1_000", "'a'", "\"s\""]

# small windows, so a mutant's edits land somewhere interesting
sys.exit(xcdiff.run("tokdump", "tokens", ALPHA, (20, 400), 6))
