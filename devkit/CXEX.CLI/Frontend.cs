// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using CXEX.Lang.Abi;
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;
using CXEX.Lang.Lexer;
using CXEX.Lang.Parsing;
using CXEX.Lang.Sema;

namespace CXEX.CLI;

/// <summary>
/// The X front end as `cxk compile` runs it: the main file with the ABI prelude
/// in front of it, every file it imports (transitively, breadth first) merged
/// into one unit, then name resolution and - if nothing has failed by then -
/// type checking. `cxk sema` prints what this produces, so the compiler and the
/// dump the X type checker is held to cannot drift apart.
/// </summary>
public sealed class Frontend
{
    public readonly DiagnosticBag Diag = new();
    public CompilationUnit Unit = new(new List<Decl>());
    public SemaContext Ctx = null!;
    public TypeChecker? Checker;   // null when an earlier error stopped it running
    public string FileName = "";

    public static Frontend Analyze(string source, bool noPrelude, string[] includeDirs)
    {
        var f = new Frontend();
        string prelude = noPrelude ? "" : AbiPrelude.Generate() + "\n";
        string full = prelude + File.ReadAllText(source);
        f.FileName = Path.GetFileName(source);

        var merged = new List<Decl>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var work = new Queue<(string path, string text, string name)>();

        // the main file carries the prepended ABI prelude; imported files do NOT
        // (those decls are already in the merged unit; re-adding collides)
        seen.Add(Path.GetFullPath(source));
        work.Enqueue((Path.GetFullPath(source), full, f.FileName));

        while (work.Count > 0)
        {
            var (path, text, name) = work.Dequeue();
            var toks = new Lexer(text, name, f.Diag).Tokenize();
            var u = new Parser(toks, f.Diag).Parse();

            foreach (var d in u.Decls)
            {
                if (d is ImportDecl imp)
                {
                    string? resolved = ResolveImport(imp.Path, path, source, includeDirs);
                    if (resolved == null)
                    {
                        f.Diag.Error($"cannot find import \"{imp.Path}\"", d.Span);
                        continue;
                    }
                    if (!seen.Add(resolved)) continue;   // already pulled in / cyclic
                    work.Enqueue((resolved, File.ReadAllText(resolved), Path.GetFileName(resolved)));
                }
                else merged.Add(d);
            }
        }

        f.Unit = new CompilationUnit(merged);
        f.Ctx = new Resolver(f.Diag).Resolve(f.Unit);
        if (!f.Diag.HasErrors)
        {
            f.Checker = new TypeChecker(f.Ctx, f.Diag);
            f.Checker.Check(f.Unit);
        }
        return f;
    }

    /// <summary>
    /// Resolve an import path: the importing file's dir, then the main source's
    /// dir, then the -I dirs, then `std/` beside cxk.exe. Returns full path or null.
    /// </summary>
    private static string? ResolveImport(string spec, string importerPath, string mainSource, string[] includeDirs)
    {
        var roots = new List<string>
        {
            Path.GetDirectoryName(importerPath) ?? ".",
            Path.GetDirectoryName(Path.GetFullPath(mainSource)) ?? ".",
        };
        roots.AddRange(includeDirs);                                  // -I dirs
        roots.Add(Path.Combine(AppContext.BaseDirectory, "std"));     // stdlib beside cxk.exe
        foreach (var r in roots)
        {
            string cand = Path.GetFullPath(Path.Combine(r, spec));
            if (File.Exists(cand)) return cand;
        }
        return null;
    }
}
