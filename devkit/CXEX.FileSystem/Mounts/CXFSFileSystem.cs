using System;
using System.Collections.Generic;
using System.IO;
using CXEX.Core.Interfaces;
using CXEX.FileSystem.Volume;
using CXEX.FileSystem.Paths;

namespace CXEX.FileSystem.Mounts;

public class CXFSFileSystem : ICXFileSystem, IDisposable
{
    private CXFSImage? _image;
    private FileStream? _hostStream;

    public string Name => "CXFS";

    public void Mount(string path)
    {
        Unmount();
        _hostStream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);

        // Assume CXFS starts at LBA 0 for testing; 
        // in production, you might need to read the XBPT table first to find the CXFS partition LBA.
        _image = new CXFSImage(_hostStream, 0);
    }

    public void Unmount()
    {
        _image?.Dispose();
        _hostStream?.Dispose();
        _image = null;
        _hostStream = null;
    }

    private int ResolvePath(string absolutePath)
    {
        if (_image == null) return -1;
        uint currentId = _image.Superblock.RootId;

        string[] parts = CXPath.Split(absolutePath);
        if (parts.Length == 0) return (int)currentId; // Root

        foreach (var part in parts)
        {
            if (part == ".") continue;
            if (part == "..")
            {
                if (_image.Manifest.TryGetValue(currentId, out var curEntry))
                    currentId = curEntry.ParentId;
                continue;
            }

            bool found = false;
            foreach (var entry in _image.Manifest.Values)
            {
                if (entry.ParentId == currentId && entry.Id != currentId &&
                    entry.Name.Equals(part, StringComparison.OrdinalIgnoreCase))
                {
                    currentId = entry.Id;
                    found = true;
                    break;
                }
            }

            if (!found) return -1;
        }

        return (int)currentId;
    }

    public bool Exists(string absolutePath) => ResolvePath(absolutePath) != -1;

    public byte[] Read(string absolutePath)
    {
        if (_image == null) throw new InvalidOperationException("No volume mounted.");

        int id = ResolvePath(absolutePath);
        if (id == -1) throw new FileNotFoundException($"File not found in CXFS: {absolutePath}");

        if (!_image.Manifest.TryGetValue((uint)id, out var entry) || !entry.IsFile)
            throw new InvalidOperationException("Target is not a file.");

        return _image.ReadFileBytes(entry);
    }

    public Stream OpenFile(string absolutePath)
    {
        // For the toolchain, returning a memory stream of the read bytes is safest
        return new MemoryStream(Read(absolutePath));
    }

    public IEnumerable<string> List(string absolutePath)
    {
        if (_image == null) yield break;

        int id = ResolvePath(absolutePath);
        if (id == -1) yield break;

        foreach (var entry in _image.Manifest.Values)
        {
            if (entry.ParentId == (uint)id && entry.Id != (uint)id && !entry.IsFree)
            {
                yield return entry.Name;
            }
        }
    }

    public void Write(string absolutePath, byte[] data)
    {
        throw new NotSupportedException("Write operations are currently not implemented in the host toolchain.");
    }

    public void Dispose() => Unmount();
}