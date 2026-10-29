// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.ComponentModel;
using System.IO;
using System.Text;
using System.Threading;
using CXEX.Lang.Ast;
using CXEX.Lang.IR;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Print the typed IR X programs lower to (roadmap step 6b2), in the layout
/// of <c>asm</c>: per program a line <c>== path</c>, every diagnostic as
/// <c>error file:line:col message</c>, then, if there were none, the IR. What
/// the lowering refuses, and every problem the verifier finds, is an
/// <c>error ir: ...</c> line instead, so a program the front end refuses is
/// told apart from one the IR cannot carry; a module the verifier refuses is
/// still printed, so the problem can be seen.
/// </summary>
public class IrCommand : Command<IrCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<SOURCES>")]
        [Description("X programs: each is lowered on its own, with what it imports.")]
        public string[] Sources { get; set; } = Array.Empty<string>();

        [CommandOption("-I|--include <DIR>")]
        [Description("Extra directory to search for imports (repeatable)")]
        public string[] IncludeDirs { get; set; } = Array.Empty<string>();

        [CommandOption("--no-prelude")]
        [Description("do not prepend the generated abi.x prelude")]
        public bool NoPrelude { get; set; }

        [CommandOption("--target <TARGET>")]
        [Description("x86_64 (the default) or x86_32: the pointer width and layout the IR is lowered for")]
        public string Target { get; set; } = "x86_64";
    }

    protected override int Execute(CommandContext context, Settings s, CancellationToken cancellationToken)
    {
        var target = XTarget.Parse(s.Target);
        if (target == null) { Console.Error.WriteLine($"error: unknown target '{s.Target}' (x86_32 or x86_64)"); return 1; }
        using var stdout = Console.OpenStandardOutput();
        int bad = 0;
        foreach (var path in s.Sources)
        {
            var fe = Frontend.Analyze(path, s.NoPrelude, s.IncludeDirs, target);
            IrModule? ir = null;
            int frontEnd = fe.Diag.Items.Count;   // what came after is the lowering's
            if (fe.Checker != null && !fe.Diag.HasErrors)
            {
                ir = new IrLowering(fe.Ctx, fe.Checker.LocalTypes, fe.Diag).Lower(fe.Unit);
                if (fe.Diag.HasErrors) ir = null;
            }
            var sb = new StringBuilder();
            sb.Append("== ").Append(path).Append('\n');
            for (int k = 0; k < fe.Diag.Items.Count; k++)
            {
                var e = fe.Diag.Items[k];
                sb.Append(k < frontEnd ? "error " : "error ir: ");
                sb.Append(e.Span.File ?? "").Append(':').Append(e.Span.Line).Append(':').Append(e.Span.Col).Append(' ');
                AstCommand.Escape(sb, e.Message, quote: false);
                sb.Append('\n');
            }
            if (fe.Diag.HasErrors) bad++;
            if (ir != null)
            {
                var problems = IrVerifier.Verify(ir);
                foreach (var p in problems) sb.Append("error ir: ").Append(p).Append('\n');
                if (problems.Count > 0) bad++;
                sb.Append(IrPrinter.Print(ir));
            }
            var bytes = Encoding.UTF8.GetBytes(sb.ToString());
            stdout.Write(bytes, 0, bytes.Length);
        }
        return bad == 0 ? 0 : 1;
    }
}
