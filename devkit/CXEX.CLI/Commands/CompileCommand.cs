using CXEX.Lang.Abi;
using CXEX.Lang.Ast;
using CXEX.Lang.CodeGen;
using CXEX.Lang.Diagnostics;
using CXEX.Lang.Lexer;
using CXEX.Lang.Parsing;
using CXEX.Lang.Sema;
using Spectre.Console;
using Spectre.Console.Cli;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Threading;

namespace CXEX.CLI.Commands;

/// <summary>
/// Compiles an X source file (.x) to an ELF, ready for `cxk build` to package as
/// CXEX. Pipeline: [abi.x prelude] + source -> Lexer -> Parser -> Resolver ->
/// TypeChecker -> X86Emitter (-> .s) -> i686-elf-gcc assemble -> link @0x400000.
/// </summary>
public class CompileCommand : Command<CompileCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<SOURCE>")]
        [Description("X source file (.x)")]
        public string Source { get; set; } = string.Empty;

        [CommandArgument(1, "<OUTPUT_ELF>")]
        [Description("output ELF path")]
        public string Output { get; set; } = string.Empty;

        [CommandOption("--ld <SCRIPT>")]
        [Description("linker script (default: a generated user script @0x400000)")]
        public string? LinkerScript { get; set; }

        [CommandOption("-I|--include <DIR>")]
        [Description("Extra directory to search for imports (repeatable, e.g. the stdlib)")]
        public string[] IncludeDirs { get; set; } = System.Array.Empty<string>();

        [CommandOption("--no-prelude")]
        [Description("do not prepend the generated abi.x prelude")]
        public bool NoPrelude { get; set; }

        [CommandOption("--emit-asm")]
        [Description("also keep the intermediate .s next to the output")]
        public bool EmitAsm { get; set; }
    }

    protected override int Execute(CommandContext context, Settings s, CancellationToken ct)
    {
        if (!File.Exists(s.Source))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] source not found: {s.Source}");
            return 1;
        }

        // 1. assemble the compilation unit (prelude + user source)
        string userSrc = File.ReadAllText(s.Source);
        string prelude = s.NoPrelude ? "" : AbiPrelude.Generate() + "\n";
        string full = prelude + userSrc;
        string fileName = Path.GetFileName(s.Source);

        // 2. front-end + analysis (whole-program: main file + transitive imports)
        var diag = new DiagnosticBag();
        var merged = new List<Decl>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var work = new Queue<(string path, string text, string name)>();

        // the main file carries the prepended ABI prelude; imported files do NOT
        // (those decls are already in the merged unit; re-adding collides)
        seen.Add(Path.GetFullPath(s.Source));
        work.Enqueue((Path.GetFullPath(s.Source), full, fileName));

        while (work.Count > 0)
        {
            var (path, text, name) = work.Dequeue();
            var toks = new Lexer(text, name, diag).Tokenize();
            var u = new Parser(toks, diag).Parse();

            foreach (var d in u.Decls)
            {
                if (d is ImportDecl imp)
                {
                    string? resolved = ResolveImport(imp.Path, path, s.Source, s.IncludeDirs);
                    if (resolved == null)
                    {
                        diag.Error($"cannot find import \"{imp.Path}\"", d.Span);
                        continue;
                    }
                    if (!seen.Add(resolved)) continue;   // already pulled in / cyclic
                    work.Enqueue((resolved, File.ReadAllText(resolved), Path.GetFileName(resolved)));
                }
                else merged.Add(d);
            }
        }

        var unit = new CompilationUnit(merged);
        var ctx = new Resolver(diag).Resolve(unit);
        TypeChecker? tc = null;
        if (!diag.HasErrors)
        {
            tc = new TypeChecker(ctx, diag);
            tc.Check(unit);
        }

        if (diag.HasErrors)
        {
            foreach (var d in diag.Items)
            {
                var color = d.Severity == Severity.Error ? "red" : "yellow";
                AnsiConsole.MarkupLine($"[grey]{Markup.Escape(d.Span.ToString())}:[/] [{color}]{d.Severity.ToString().ToLowerInvariant()}:[/] {Markup.Escape(d.Message)}");
            }
            AnsiConsole.MarkupLine($"[red]compilation failed[/] ({CountErrors(diag)} error(s))");
            return 1;
        }
        AnsiConsole.MarkupLine($"[green]X:[/] analyzed {fileName} ({unit.Decls.Count} decls)");

        // 3. codegen -> asm
        string asm = new X86Emitter(ctx, tc!.LocalTypes, diag).Emit(unit);
        string asmPath = Path.ChangeExtension(s.Output, ".s");
        File.WriteAllText(asmPath, asm);
        AnsiConsole.MarkupLine($"[cyan]X:[/] emitted {Path.GetFileName(asmPath)}");

        // 4. assemble + link via the cross toolchain
        string objPath = Path.ChangeExtension(s.Output, ".o");
        if (!Wrappers.GccTool.Compile(asmPath, objPath))
        {
            AnsiConsole.MarkupLine("[red]error:[/] assembling the emitted .s failed");
            return 1;
        }

        string ld = s.LinkerScript ?? WriteDefaultScript(s.Output);
        if (!Wrappers.GccTool.Link(new[] { objPath }, s.Output, ld))
        {
            AnsiConsole.MarkupLine("[red]error:[/] linking failed");
            return 1;
        }

        if (!s.EmitAsm) { TryDelete(asmPath); }
        TryDelete(objPath);
        AnsiConsole.MarkupLine($"[green]done:[/] {s.Output}");
        AnsiConsole.MarkupLine($"[grey]next:[/] cxk build \"{s.Output}\" out.xuex --type user");
        return 0;
    }

    private static int CountErrors(DiagnosticBag d)
    {
        int n = 0; foreach (var i in d.Items) if (i.Severity == Severity.Error) n++; return n;
    }

    /// <summary>
    /// Resolve an import path: the importing file's dir, then the main source's
    /// dir, then `std/` beside cxk.exe. Returns full path or null.
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

    private static string WriteDefaultScript(string output)
    {
        // PHDRS keeps code and data in separate LOAD segments with distinct
        // permissions: text = R+X (FLAGS 5), data = R+W (FLAGS 6). Without this,
        // ld folds everything into one RWX segment (writable code pages) and warns
        // "LOAD segment with RWX permissions". rodata rides with text (read-only,
        // no write needed); .data/.bss ride with the writable segment.
        string ld = Path.ChangeExtension(output, ".ld");
        File.WriteAllText(ld,
            "ENTRY(_start)\n" +
            "PHDRS {\n" +
            "    text PT_LOAD FLAGS(5);   /* R + X */\n" +
            "    data PT_LOAD FLAGS(6);   /* R + W */\n" +
            "}\n" +
            "SECTIONS {\n" +
            "    . = 0x00400000;\n" +
            "    .text   : { *(.text*) }         :text\n" +
            "    .rodata : { *(.rodata*) }       :text\n" +
            "    . = ALIGN(0x1000);\n" +
            "    .data   : { *(.data*) }         :data\n" +
            "    .bss    : { *(.bss*) *(COMMON) } :data\n" +
            "\n" +
            "    /* Orphan sections MUST be discarded, not left to ld's placement\n" +
            "       heuristics. A toolchain that emits anything this script does not\n" +
            "       name - a build-id note is the common one, since gcc configured\n" +
            "       with --enable-linker-build-id adds .note.gnu.build-id by default -\n" +
            "       gets it placed at 0x00400000 ahead of .text. That displaces the\n" +
            "       entry point, and because the orphan is also folded into a segment,\n" +
            "       both LOAD headers end up with vaddr 0x00400000 overlapping each\n" +
            "       other. The CXEX loader then maps the image wrong and the process\n" +
            "       dies with a page fault the moment it runs. Nothing warns.\n" +
            "       None of these sections mean anything to a freestanding CXEX image. */\n" +
            "    /DISCARD/ : {\n" +
            "        *(.note*)\n" +
            "        *(.comment)\n" +
            "        *(.eh_frame*)\n" +
            "        *(.gnu.build-id)\n" +
            "        *(.gnu_debuglink)\n" +
            "    }\n" +
            "}\n");
        return ld;
    }

    private static void TryDelete(string p) { try { if (File.Exists(p)) File.Delete(p); } catch { } }
}