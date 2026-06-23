using CXEX.Lang.Diagnostics;

namespace CXEX.Lang.Lexer;

/// <summary>A lexed token. Text is the exact source slice; for IntLiteral, Value holds the parsed number.</summary>
public readonly record struct Token(TokenKind Kind, string Text, SourceSpan Span, ulong Value = 0)
{
    public override string ToString() => $"{Kind}('{Text}')";
}