using System;
using System.Linq;

namespace CXEX.Core.FileSystems;

public static class CXPath
{
    public const char Separator = '/';

    public static string Normalize(string path)
    {
        if (string.IsNullOrWhiteSpace(path)) return "/";

        // Convert backslashes to forward slashes and trim
        string normalized = path.Replace('\\', Separator).Trim();

        // Ensure it starts with a separator
        if (!normalized.StartsWith(Separator))
            normalized = Separator + normalized;

        return normalized;
    }

    public static string[] Split(string path)
    {
        return Normalize(path)
            .Split(new[] { Separator }, StringSplitOptions.RemoveEmptyEntries);
    }

    public static string GetDirectoryName(string path)
    {
        var normalized = Normalize(path);
        int lastSep = normalized.LastIndexOf(Separator);
        return lastSep <= 0 ? "/" : normalized.Substring(0, lastSep);
    }
}