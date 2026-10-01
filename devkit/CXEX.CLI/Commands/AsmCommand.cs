using System;
using System.ComponentModel;
using System.IO;
using System.Text;
using System.Threading;
using CXEX.Lang.CodeGen;
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
    }

    protected override int Execute(CommandContext context, Settings s, CancellationToken cancellationToken)
    {
        using var stdout = Console.OpenStandardOutput();
        foreach (var path in s.Sources)
        {
            var fe = Frontend.Analyze(path, s.NoPrelude, s.IncludeDirs);
            string? asm = null;
            if (fe.Checker != null && !fe.Diag.HasErrors)
            {
                asm = new X86Emitter(fe.Ctx, fe.Checker.LocalTypes, fe.Diag) { Library = s.Object }.Emit(fe.Unit);
                if (fe.Diag.HasErrors) asm = null;
            }
            var sb = new StringBuilder();
            foreach (var b in Encoding.UTF8.GetBytes("== " + path)) sb.Append((char)b);
            sb.Append('\n');
            foreach (var e in fe.Diag.Items)
            {
                sb.Append("error ");
                foreach (var b in Encoding.UTF8.GetBytes(e.Span.File ?? "")) sb.Append((char)b);
                sb.Append(':').Append(e.Span.Line).Append(':').Append(e.Span.Col).Append(' ');
                AstCommand.Escape(sb, e.Message, quote: false);
                sb.Append('\n');
            }
            if (asm != null) foreach (var b in Encoding.UTF8.GetBytes(asm)) sb.Append((char)b);
            var bytes = Encoding.Latin1.GetBytes(sb.ToString());
            stdout.Write(bytes, 0, bytes.Length);
        }
        return 0;
    }
}
