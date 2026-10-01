using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using CXEX.Core.Interfaces;

namespace CXEX.FileSystem.Mounts;

public class CXMemoryFileSystem : ICXFileSystem
{
    private readonly Dictionary<string, byte[]> _files = new(StringComparer.OrdinalIgnoreCase);

    public string Name => "MemoryFS";

    public void Mount(string path) { /* No-op for RAM disk */ }
    public void Unmount() => _files.Clear();

    public bool Exists(string absolutePath) => _files.ContainsKey(absolutePath);

    public byte[] Read(string absolutePath)
    {
        if (_files.TryGetValue(absolutePath, out var data)) return data;
        throw new FileNotFoundException($"Virtual file not found: {absolutePath}");
    }

    public void Write(string absolutePath, byte[] data) => _files[absolutePath] = data;

    public IEnumerable<string> List(string absolutePath)
    {
        string prefix = absolutePath.EndsWith("/") ? absolutePath : absolutePath + "/";
        return _files.Keys.Where(k => k.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
    }

    public Stream OpenFile(string absolutePath) => new MemoryStream(Read(absolutePath));
}