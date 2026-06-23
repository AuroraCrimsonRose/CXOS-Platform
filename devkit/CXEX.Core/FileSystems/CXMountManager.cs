using System.Collections.Generic;

namespace CXEX.Core.FileSystems;

public class CXMountManager
{
    private readonly Dictionary<string, ICXFileSystem> _mounts = new();

    public void Mount(string driveLetter, ICXFileSystem fs)
    {
        _mounts[driveLetter.ToUpper()] = fs;
    }

    public Stream Open(string path)
    {
        // Simple resolution logic: "C:/system/kernel.xkex" -> Drive C
        var parts = path.Split(':', 2);
        if (parts.Length < 2 || !_mounts.TryGetValue(parts[0].ToUpper(), out var fs))
            throw new FileNotFoundException($"Drive not mounted: {path}");

        return fs.OpenFile(parts[1]);
    }
}