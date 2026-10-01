using CXEX.Lang.Diagnostics;

namespace CXEX.Lang.Lexer;

/// <summary>A lexed token. Text is the exact source slice; for IntLiteral, Value holds the parsed number, up to the 128 bits X's widest type can hold, and Suffix the type it was written with (`1u64`), if any.</summary>
public readonly record struct Token(TokenKind Kind, string Text, SourceSpan Span, UInt128 Value = default, string? Suffix = null)
{
    public override string ToString() => $"{Kind}('{Text}')";
}