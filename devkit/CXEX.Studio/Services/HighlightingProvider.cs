using System;
using System.Collections.Generic;
using System.Xml;
using Avalonia.Platform;
using AvaloniaEdit.Highlighting;
using AvaloniaEdit.Highlighting.Xshd;

namespace CXEX.Studio.Services;

/// <summary>
/// Loads CX's custom high-contrast .xshd highlighting (bundled as AvaloniaResource)
/// and caches it. Built-in AvaloniaEdit definitions are tuned for light backgrounds,
/// so we ship our own bright/dark-friendly ones. Falls back to the built-in def if
/// the resource can't be loaded. (X Native grammars plug in here later.)
/// </summary>
public static class HighlightingProvider
{
    private static readonly Dictionary<string, IHighlightingDefinition?> _cache = new();

    public static IHighlightingDefinition? ForExtension(string ext) => ext.ToLowerInvariant() switch
    {
        ".c" or ".h" or ".cpp" or ".hpp" => LoadXshd("CX-C", "C++"),
        ".asm" or ".nasm" => HighlightingManager.Instance.GetDefinition("Assembly"),
        // ".xfxn" or ".xfxr" or ".xfxh" => LoadXshd("CX-X", null),   // Phase 6
        _ => null
    };

    private static IHighlightingDefinition? LoadXshd(string name, string? fallbackBuiltin)
    {
        if (_cache.TryGetValue(name, out var cached)) return cached;

        IHighlightingDefinition? def = null;
        try
        {
            var uri = new Uri($"avares://CXEX.Studio/Assets/Highlighting/{name}.xshd");
            using var stream = AssetLoader.Open(uri);
            using var reader = XmlReader.Create(stream);
            def = HighlightingLoader.Load(reader, HighlightingManager.Instance);
        }
        catch
        {
            if (fallbackBuiltin is not null)
                def = HighlightingManager.Instance.GetDefinition(fallbackBuiltin);
        }

        _cache[name] = def;
        return def;
    }
}