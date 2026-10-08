// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using Xunit;

namespace CXEX.Tests.Differential;

/// <summary>
/// The lexer and parser written in X, against the C# ones, token for token and
/// node for node. Ports <c>devkit/tests/xc/lexdiff.py</c> and
/// <c>parsediff.py</c> (HARDENING_PLAN D1).
/// </summary>
[Trait(Categories.Key, Categories.Differential)]
public class LexParseDiffTests
{
    /// <summary>
    /// Fragments aimed at what a lexer gets wrong: comment and string openers
    /// that never close, numeric prefixes and separators, escapes, non-ASCII,
    /// and CRLF.
    /// </summary>
    private static readonly string[] LexAlphabet =
        "\"'\\_0123456789xXuUabcdefz{}[]()<>=!&|^~+-*/%.,;:@ \t\n"
            .Select(c => c.ToString())
            .Concat(new[]
            {
                "//", "/*", "*/", "0x", "u8", "i128", "\\x", "\\n",
                "é", "😀", "\r\n", "1_000", "'a'", "\"s\"",
            })
            .ToArray();

    /// <summary>
    /// Fragments aimed at the parser: whole keywords and constructs, so a
    /// mutant is likely to be nearly-valid rather than obvious noise.
    /// </summary>
    private static readonly string[] ParseAlphabet =
        "{}[]()<>=!&|^~+-*/%.,;:@ \n"
            .Select(c => c.ToString())
            .Concat(new[]
            {
                "fn ", "struct ", "enum ", "global ", "const ", "extern ", "type ", "import ", "let ",
                "if ", "else ", "while ", "return ", "break;", "continue;", "defer ", "switch ", "case ",
                "as ", "sizeof ", "true", "false",
                "->", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "+=", "-=", "<<=", "|=",
                "x", "y.z", "f(a)", "e.v(n)", "pt { x: 1 }", "[1, 2]", "*user u8", "[4]u32",
                "fn(u32) -> bool", "u128", "1u64", "'c'", "\"s\"",
                "@attr", "@a(k = 1)", "{ }", "( )", "a = b;", "x: i32", "4294967296", "é",
            })
            .ToArray();

    [Fact]
    public void Lexer_in_X_agrees_with_the_C_sharp_lexer() =>
        XcDumpHarness.Compare("tokdump", "tokens", LexAlphabet, (20, 400), edits: 6);

    [Fact]
    public void Parser_in_X_agrees_with_the_C_sharp_parser() =>
        XcDumpHarness.Compare("astdump", "ast", ParseAlphabet, (40, 3000), edits: 8, lineOps: true);
}
