using System;
using System.Linq;

namespace CXEX.FileSystem.Paths;

public static class CXPath
{
    public static string[] Split(string path)
    {
        if (string.IsNullOrWhiteSpace(path)) return Array.Empty<string>();
        return path.Split('/', StringSplitOptions.RemoveEmptyEntries);
    }

    public static string GetFileName(string path)
    {
        var parts = Split(path);
        return parts.Length > 0 ? parts.Last() : string.Empty;
    }
}