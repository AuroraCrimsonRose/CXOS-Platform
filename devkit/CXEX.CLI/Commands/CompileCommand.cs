// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.CLI.Infrastructure;
using CXEX.Tools;
﻿using CXEX.Lang.Abi;
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
/// TypeChecker -> X86Emitter (-> .s) -> clang assemble -> ld.lld link @0x400000.
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

        [CommandOption("--object")]
        [Description("write a linkable object instead of a program: every function and global exported, no entry point, no link step. OUTPUT is the .o")]
        public bool Object { get; set; }
    }

    protected override int Execute(CommandContext context, Settings s, CancellationToken ct)
    {
        if (!File.Exists(s.Source))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] source not found: {s.Source}");
            return 1;
        }

        // 1-2. the front end: prelude + source + imports, resolved and checked
        var fe = Frontend.Analyze(s.Source, s.NoPrelude, s.IncludeDirs);
        var diag = fe.Diag;
        var unit = fe.Unit;
        var ctx = fe.Ctx;
        var tc = fe.Checker;
        string fileName = fe.FileName;

        if (diag.HasErrors)
        {
            /* Plain lines, not Spectre markup: Spectre wraps at the console
               width, which is 80 when the output is a build log, and split an
               error message across lines where neither an editor nor a log
               search could find it whole. */
            foreach (var d in diag.Items)
                Console.WriteLine($"{d.Span}: {d.Severity.ToString().ToLowerInvariant()}: {d.Message}");
            AnsiConsole.MarkupLine($"[red]compilation failed[/] ({CountErrors(diag)} error(s))");
            return 1;
        }
        AnsiConsole.MarkupLine($"[green]X:[/] analyzed {fileName} ({unit.Decls.Count} decls)");

        // 3. codegen -> asm
        string asm = new X86Emitter(ctx, tc!.LocalTypes, diag) { Library = s.Object }.Emit(unit);

        /* Codegen reports too, and its diagnostics were being thrown away: the
           only HasErrors check was above, before the emitter had run. Anything
           the emitter refused to emit - an unsupported operation, a construct
           it cannot express safely - produced a clean "done" and an object
           file with the offending expression quietly missing. */
        if (diag.HasErrors)
        {
            foreach (var d in diag.Items)
                if (d.Severity == Severity.Error) Console.WriteLine($"{d.Span}: error: {d.Message}");
            AnsiConsole.MarkupLine($"[red]compilation failed[/] ({CountErrors(diag)} error(s))");
            return 1;
        }

        string asmPath = Path.ChangeExtension(s.Output, ".s");
        File.WriteAllText(asmPath, asm);
        AnsiConsole.MarkupLine($"[cyan]X:[/] emitted {Path.GetFileName(asmPath)}");

        // 4a. an object stops at assembly: whatever links it supplies the rest
        if (s.Object)
        {
            if (!CliTools.Report(ClangTool.Compile(asmPath, s.Output, options: CliTools.Options($"clang (assemble): {Path.GetFileName(asmPath)}"))))
            {
                AnsiConsole.MarkupLine("[red]error:[/] assembling the emitted .s failed");
                return 1;
            }
            if (!s.EmitAsm) { TryDelete(asmPath); }
            AnsiConsole.MarkupLine($"[green]done:[/] {s.Output}");
            return 0;
        }

        // 4. assemble + link via the cross toolchain
        string objPath = Path.ChangeExtension(s.Output, ".o");
        if (!CliTools.Report(ClangTool.Compile(asmPath, objPath, options: CliTools.Options($"clang (assemble): {Path.GetFileName(asmPath)}"))))
        {
            AnsiConsole.MarkupLine("[red]error:[/] assembling the emitted .s failed");
            return 1;
        }

        string ld = s.LinkerScript ?? WriteDefaultScript(s.Output);
        if (!CliTools.Report(ClangTool.Link(new[] { objPath }, s.Output, ld, options: CliTools.Options($"ld.lld (link): {Path.GetFileName(s.Output)}"))))
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
            "       name - a build-id note is the common one, since a linker configured\n" +
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