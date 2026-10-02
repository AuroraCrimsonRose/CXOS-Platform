#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
# Differential test: the X code generator written in X (CXK os/xc/emit.xfxn)
# against the C# one (CXEX.Lang/CodeGen). Both compile the same whole programs
# - the prelude, the file, everything it imports - and print the diagnostics
# that stop the compile or, if there are none, the assembly `cxk compile`
# writes. The outputs must be identical, character for character.
#
#   python3 tests/xc/asmdiff.py [mutants] [seed]
#
# The corpus and the mutants are semadiff.py's: every X file in CXK and in
# tests/lang, and versions of them changed a few tokens at a time. The ones
# that still type check are compiled; the rest are compared on their errors.
# Requirements and SABOTAGE=1: see xcdiff.py.
import sys, progdiff

sys.exit(progdiff.run("asmdump", "asm", "\n.text\n"))
