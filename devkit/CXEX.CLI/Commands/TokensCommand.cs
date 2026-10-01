using System;
using System.ComponentModel;
using System.IO;
using System.Text;
using System.Threading;
using CXEX.Lang.Diagnostics;
using CXEX.Lang.Lexer;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Print the tokens of X source files, one per line:
/// <c>Kind line:col payload</c> under a <c>== path</c> header per file.
///
/// <para>The same format as <c>tokdump</c>, the lexer written in X (CXK os/xc), so
/// the two can be compared with a diff - which is how the X one is held to this
/// one. The payload is an identifier's text, an integer's value in decimal plus
/// its suffix, a string's decoded bytes in hex, or <c>-</c>.</para>
/// </summary>
public class TokensCommand : Command<TokensCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<FILES>")]
        [Description("X source files.")]
        public string[] Files { get; set; } = Array.Empty<string>();
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        var sb = new StringBuilder();
        foreach (var path in settings.Files)
        {
            sb.Append("== ").Append(path).Append('\n');
            var toks = new Lexer(File.ReadAllText(path), Path.GetFileName(path), new DiagnosticBag()).Tokenize();
            foreach (var t in toks)
            {
                sb.Append(t.Kind).Append(' ').Append(t.Span.Line).Append(':').Append(t.Span.Col).Append(' ');
                switch (t.Kind)
                {
                    case TokenKind.Identifier: sb.Append(t.Text); break;
                    case TokenKind.IntLiteral:
                        sb.Append(t.Value.ToString());
                        if (t.Suffix != null) sb.Append(' ').Append(t.Suffix);
                        break;
                    case TokenKind.StringLiteral:
                        var bytes = Encoding.UTF8.GetBytes(t.Text);
                        if (bytes.Length == 0) sb.Append('-');
                        foreach (var b in bytes) sb.Append(b.ToString("x2"));
                        break;
                    default: sb.Append('-'); break;
                }
                sb.Append('\n');
            }
            Console.Out.Write(sb.ToString());
            sb.Clear();
        }
        return 0;
    }
}
