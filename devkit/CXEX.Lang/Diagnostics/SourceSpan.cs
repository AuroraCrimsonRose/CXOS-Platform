namespace CXEX.Lang.Diagnostics;

/// <summary>A half-open [Start,End) byte range in a source file, with line/col for messages.</summary>
public readonly record struct SourceSpan(string File, int Start, int End, int Line, int Col)
{
    public override string ToString() => $"{File}:{Line}:{Col}";
}