using System.Collections.Generic;

namespace CXEX.Lang.Diagnostics;

public enum Severity { Error, Warning, Note }

public sealed record Diagnostic(Severity Severity, string Message, SourceSpan Span)
{
    public override string ToString() => $"{Span}: {Severity.ToString().ToLowerInvariant()}: {Message}";
}

/// <summary>Collects diagnostics across a compile. HasErrors gates later phases.</summary>
public sealed class DiagnosticBag
{
    private readonly List<Diagnostic> _items = new();
    public IReadOnlyList<Diagnostic> Items => _items;
    public bool HasErrors { get; private set; }

    public void Error(string msg, SourceSpan span) { _items.Add(new(Severity.Error, msg, span)); HasErrors = true; }
    public void Warning(string msg, SourceSpan span) => _items.Add(new(Severity.Warning, msg, span));
}