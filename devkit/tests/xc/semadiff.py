#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
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
import sys, progdiff

sys.exit(progdiff.run("semadump", "sema", "checked yes"))
