using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace CXEX.Core.FileSystems.CXFS;

public class CXFSFileSystem : ICXFileSystem, IDisposable
{
    private CXFSImage? _volumeImage;
    private Stream? _managedStream;

    // Satisfies ICXFileSystem.Name
    public string Name => "CXFS";

    // Standard constructor for system stream attachments
    public CXFSFileSystem(Stream driveStream)
    {
        _volumeImage = new CXFSImage(driveStream);
    }

    // Parametere-less constructor allowing standalone interface instantiations before mounting
    public CXFSFileSystem()
    {
    }

    // Satisfies ICXFileSystem.Mount(string)
    public void Mount(string path)
    {
        _volumeImage?.Dispose();
        _managedStream?.Dispose();

        _managedStream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        _volumeImage = new CXFSImage(_managedStream);
    }

    // Satisfies ICXFileSystem.Unmount()
    public void Unmount()
    {
        Dispose();
    }

    public uint RootDirectoryId => _volumeImage?.Superblock.RootId ?? 0;

    public string GetEntryName(CxfsManifestEntry entry)
    {
        unsafe
        {
            // FIX CS0213: entry.Name is an already fixed expression since 'entry' is a stack local.
            // It decays directly into a byte* parameter safely without an explicit 'fixed' statement.
            return Encoding.UTF8.GetString(entry.Name, entry.NameLen);
        }
    }

    // Satisfies ICXFileSystem.Exists(string)
    public bool Exists(string absolutePath)
    {
        if (_volumeImage == null) return false;
        return ResolvePathToId(absolutePath) != -1;
    }

    // Satisfies ICXFileSystem.Read(string)
    public byte[] Read(string absolutePath)
    {
        if (_volumeImage == null) throw new InvalidOperationException("No active volume mounted.");

        int id = ResolvePathToId(absolutePath);
        if (id == -1) throw new FileNotFoundException($"The requested path does not exist: {absolutePath}");

        using Stream fileStream = OpenFile((uint)id);
        using MemoryStream ms = new MemoryStream();
        fileStream.CopyTo(ms);
        return ms.ToArray();
    }

    // Satisfies ICXFileSystem.Write(string, byte[])
    public void Write(string absolutePath, byte[] data)
    {
        throw new NotSupportedException("Write operations are currently unimplemented for the v2 system inspector layer.");
    }

    // Satisfies ICXFileSystem.List(string)
    public IEnumerable<string> List(string absolutePath)
    {
        if (_volumeImage == null) yield break;

        int id = ResolvePathToId(absolutePath);
        if (id == -1) throw new DirectoryNotFoundException($"The target directory structure was not found: {absolutePath}");

        foreach (var entry in ListDirectory((uint)id))
        {
            yield return GetEntryName(entry);
        }
    }

    public IEnumerable<CxfsManifestEntry> ListDirectory(uint directoryId)
    {
        if (_volumeImage == null) yield break;

        foreach (var entry in _volumeImage.Manifest.Values)
        {
            if (entry.ParentId == directoryId && entry.Id != directoryId)
            {
                yield return entry;
            }
        }
    }

    public int ResolvePathToId(string absolutePath)
    {
        if (_volumeImage == null) return -1;
        if (string.IsNullOrWhiteSpace(absolutePath) || absolutePath == "/")
            return (int)RootDirectoryId;

        string[] components = absolutePath.Split('/', StringSplitOptions.RemoveEmptyEntries);
        uint currentId = RootDirectoryId;

        foreach (var component in components)
        {
            if (component == ".") continue;
            if (component == "..")
            {
                currentId = _volumeImage.Manifest[currentId].ParentId;
                continue;
            }

            bool foundNext = false;
            foreach (var child in ListDirectory(currentId))
            {
                string name = GetEntryName(child);
                if (string.Equals(name, component, StringComparison.OrdinalIgnoreCase))
                {
                    currentId = child.Id;
                    foundNext = true;
                    break;
                }
            }

            if (!foundNext) return -1;
        }

        return (int)currentId;
    }

    public string GetAbsolutePath(uint entryId)
    {
        if (_volumeImage == null) return "/";
        if (entryId == RootDirectoryId) return "/";

        List<string> segments = new();
        uint current = entryId;

        while (current != RootDirectoryId)
        {
            if (!_volumeImage.Manifest.TryGetValue(current, out var entry))
                break;

            segments.Add(GetEntryName(entry));
            current = entry.ParentId;
        }

        segments.Reverse();
        return "/" + string.Join('/', segments);
    }

    public Stream OpenFile(uint fileId)
    {
        if (_volumeImage == null) throw new InvalidOperationException("No volume image mounted.");
        if (!_volumeImage.Manifest.TryGetValue(fileId, out var entry))
            throw new FileNotFoundException($"No manifest structure entry mapped to system layout index ID: {fileId}");

        if (entry.Type != CXFSConstants.TypeFile)
            throw new ArgumentException("The targeted file ID points to a structural container location rather than a data payload stream.");

        return _volumeImage.CreateFileStream(entry);
    }

    public void Dispose()
    {
        _volumeImage?.Dispose();
        _managedStream?.Dispose();
        _volumeImage = null;
        _managedStream = null;
    }
}