// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Text;
using System.Threading;
using CXEX.Lang.Ast;
using CXEX.Lang.CodeGen;
using CXEX.Lang.IR;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Print the assembly `cxk compile` would generate for X programs, or the
/// diagnostics that stop it.
///
/// <para>The same format as <c>asmdump</c>, the code generator written in X (CXK
/// os/xc), so the two can be compared with a diff. Per program: a line
/// <c>== path</c>; then every diagnostic as <c>error file:line:col message</c>;
/// then, if there were none, the assembly text exactly as compile writes it.</para>
/// </summary>
public class AsmCommand : Command<AsmCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<SOURCES>")]
        [Description("X programs: each is compiled on its own, with what it imports.")]
        public string[] Sources { get; set; } = Array.Empty<string>();

        [CommandOption("-I|--include <DIR>")]
        [Description("Extra directory to search for imports (repeatable)")]
        public string[] IncludeDirs { get; set; } = Array.Empty<string>();

        [CommandOption("--no-prelude")]
        [Description("do not prepend the generated abi.x prelude")]
        public bool NoPrelude { get; set; }

        [CommandOption("--object")]
        [Description("as `compile --object`: every function and global exported, no entry point")]
        public bool Object { get; set; }

        [CommandOption("--target <TARGET>")]
        [Description("x86_32 (the default: the emitter, from the tree) or x86_64 (the System V back end on the IR)")]
        public string Target { get; set; } = "x86_32";
    }

    protected override int Execute(CommandContext context, Settings s, CancellationToken cancellationToken)
    {
        var target = XTarget.Parse(s.Target);
        if (target == null) { Console.Error.WriteLine($"error: unknown target '{s.Target}' (x86_32 or x86_64)"); return 1; }
        using var stdout = Console.OpenStandardOutput();
        foreach (var path in s.Sources)
        {
            var fe = Frontend.Analyze(path, s.NoPrelude, s.IncludeDirs, target);
            string? asm = null;
            int frontEnd = int.MaxValue;   // on x86-64, what came after is the lowering's, marked as `cxk ir` marks it
            var problems = new List<string>();
            if (fe.Checker != null && !fe.Diag.HasErrors)
            {
                if (target == XTarget.X86_64)
                {
                    /* As `compile --target x86_64`: lowered, verified, emitted.
                       A module the verifier refuses has no assembly. */
                    frontEnd = fe.Diag.Items.Count;
                    var ir = new IrLowering(fe.Ctx, fe.Checker.LocalTypes, fe.Diag).Lower(fe.Unit);
                    if (!fe.Diag.HasErrors) problems = IrVerifier.Verify(ir);
                    if (!fe.Diag.HasErrors && problems.Count == 0)
                        asm = new X64Emitter(ir) { Library = s.Object, SyscallPage = !s.NoPrelude }.Emit();
                }
                else
                {
                    asm = new X86Emitter(fe.Ctx, fe.Checker.LocalTypes, fe.Diag) { Library = s.Object, SyscallPage = !s.NoPrelude }.Emit(fe.Unit);
                    if (fe.Diag.HasErrors) asm = null;
                }
            }
            var sb = new StringBuilder();
            foreach (var b in Encoding.UTF8.GetBytes("== " + path)) sb.Append((char)b);
            sb.Append('\n');
            for (int k = 0; k < fe.Diag.Items.Count; k++)
            {
                var e = fe.Diag.Items[k];
                sb.Append(k < frontEnd ? "error " : "error ir: ");
                foreach (var b in Encoding.UTF8.GetBytes(e.Span.File ?? "")) sb.Append((char)b);
                sb.Append(':').Append(e.Span.Line).Append(':').Append(e.Span.Col).Append(' ');
                AstCommand.Escape(sb, e.Message, quote: false);
                sb.Append('\n');
            }
            foreach (var p in problems)
                foreach (var b in Encoding.UTF8.GetBytes("error ir: " + p + "\n")) sb.Append((char)b);
            if (asm != null) foreach (var b in Encoding.UTF8.GetBytes(asm)) sb.Append((char)b);
            var bytes = Encoding.Latin1.GetBytes(sb.ToString());
            stdout.Write(bytes, 0, bytes.Length);
        }
        return 0;
    }
}
