using CXEX.Lang.Abi;
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

        // 2. front-end + analysis
        var diag = new DiagnosticBag();
        var tokens = new Lexer(full, fileName, diag).Tokenize();
        var unit = new Parser(tokens, diag).Parse();
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
        AnsiConsole.MarkupLine($"[grey]next:[/] cxk build \"{s.Output}\" out.xcex --type user");
        return 0;
    }

    private static int CountErrors(DiagnosticBag d)
    {
        int n = 0; foreach (var i in d.Items) if (i.Severity == Severity.Error) n++; return n;
    }

    private static string WriteDefaultScript(string output)
    {
        string ld = Path.ChangeExtension(output, ".ld");
        File.WriteAllText(ld,
            "ENTRY(_start)\n" +
            "SECTIONS {\n" +
            "    . = 0x00400000;\n" +
            "    .text   : { *(.text*) }\n" +
            "    .rodata : { *(.rodata*) }\n" +
            "    .data   : { *(.data*) }\n" +
            "    .bss    : { *(.bss*) *(COMMON) }\n" +
            "}\n");
        return ld;
    }

    private static void TryDelete(string p) { try { if (File.Exists(p)) File.Delete(p); } catch { } }
}