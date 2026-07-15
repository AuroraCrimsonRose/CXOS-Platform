using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using CXEX.Core.Utilities;        // MemoryPrimitives
using CXEX.FileSystem.Volume;     // CXFSSuperblock, CXFSEntry, CXFSParser

namespace CXEX.FileSystem.Mounts;

/// <summary>
/// Host-side CXFS mutation: rename, make-directory, and delete (recursive).
/// Mirrors the kernel (cxfs.c) byte-for-byte: entries pack 16/block contiguously
/// at manifest_start, the allocation bitmap is LSB-first, directories hold no data
/// (the tree is derived from parent_id), and block numbers are volume-relative.
/// Requires a READ-WRITE stream over the disk image. Does not create/write file
/// content (no extent allocation) - by design.
/// </summary>
public sealed class CXFSWriter : IDisposable
{
    private const int BS = 4096;   // block size
    private const int ES = 256;    // entry size
    private const int NAME_OFF = 12, NAME_LEN = 64;

    private readonly Stream _disk;
    private readonly CXFSSuperblock _sb;
    private readonly long _volumeBase;

    public CXFSWriter(Stream disk, CXFSSuperblock sb, ulong partitionBaseLba)
    {
        _disk = disk;
        _sb = sb;
        _volumeBase = (long)partitionBaseLba * 512;
    }

    private long EntryOffset(uint id) => _volumeBase + (long)_sb.ManifestStart * BS + (long)id * ES;
    private static ulong Now() => (ulong)DateTimeOffset.UtcNow.ToUnixTimeSeconds();

    private byte[] ReadEntryRaw(uint id)
    {
        var buf = new byte[ES];
        _disk.Position = EntryOffset(id);
        _disk.ReadExactly(buf, 0, ES);
        return buf;
    }

    private void WriteEntryRaw(uint id, byte[] entry)
    {
        _disk.Position = EntryOffset(id);
        _disk.Write(entry, 0, ES);
        _disk.Flush();
    }

    private byte[] ReadManifestRegion()
    {
        int len = (int)(_sb.ManifestBlocks * (uint)BS);
        var buf = new byte[len];
        _disk.Position = _volumeBase + (long)_sb.ManifestStart * BS;
        _disk.ReadExactly(buf, 0, len);
        return buf;
    }

    public CXFSEntry ReadEntry(uint id) => CXFSParser.ParseEntry(ReadEntryRaw(id), 0);

    /// <summary>First free manifest slot id (skips root 0), or -1 if the manifest is full.</summary>
    private int AllocEntry()
    {
        var m = ReadManifestRegion();
        for (uint id = 1; id < _sb.ManifestCount; id++)
            if (CXFSParser.ParseEntry(m, (int)id * ES).IsFree) return (int)id;
        return -1;
    }

    /// <summary>Case-insensitive child lookup; returns id or -1.</summary>
    public int FindInDir(uint parentId, string name)
    {
        var m = ReadManifestRegion();
        for (uint id = 0; id < _sb.ManifestCount; id++)
        {
            var e = CXFSParser.ParseEntry(m, (int)id * ES);
            if (!e.IsFree && e.ParentId == parentId &&
                string.Equals(e.Name, name, StringComparison.OrdinalIgnoreCase))
                return (int)id;
        }
        return -1;
    }

    private List<uint> ChildrenOf(uint parentId)
    {
        var kids = new List<uint>();
        var m = ReadManifestRegion();
        for (uint id = 0; id < _sb.ManifestCount; id++)
        {
            var e = CXFSParser.ParseEntry(m, (int)id * ES);
            if (!e.IsFree && id != parentId && e.ParentId == parentId) kids.Add(id);
        }
        return kids;
    }

    /// <summary>spaces -> '_'; reject empty, >=64 chars, '/', and control chars. Matches cxfs_normalize_name.</summary>
    public static bool TryNormalizeName(string input, out string normalized)
    {
        normalized = "";
        if (string.IsNullOrEmpty(input) || input.Length >= NAME_LEN) return false;
        var c = input.ToCharArray();
        for (int i = 0; i < c.Length; i++)
        {
            if (c[i] == ' ') c[i] = '_';
            else if (c[i] == '/') return false;
            else if (c[i] < 0x20) return false;
        }
        normalized = new string(c);
        return true;
    }

    // ---- operations ----

    /// <summary>Create a directory under parentId. Returns new id, or -1 (bad name / exists / full / parent not a dir).</summary>
    public int CreateDirectory(uint parentId, string name)
    {
        if (!TryNormalizeName(name, out var nm)) return -1;
        if (!ReadEntry(parentId).IsDirectory) return -1;
        if (FindInDir(parentId, nm) >= 0) return -1;

        int id = AllocEntry();
        if (id < 0) return -1;

        var b = new byte[ES];
        MemoryPrimitives.WriteU32(b, 0, (uint)id);
        MemoryPrimitives.WriteU32(b, 4, parentId);
        b[8] = 2;                       // CXFS_TYPE_DIR
        b[10] = (byte)nm.Length;        // name_len
        var nameBytes = Encoding.ASCII.GetBytes(nm);
        Array.Copy(nameBytes, 0, b, NAME_OFF, Math.Min(nameBytes.Length, NAME_LEN - 1));
        MemoryPrimitives.WriteU16(b, 156, 0x1ED);   // CXFS_PERM_DIR_DEFAULT (0755)
        ulong now = Now();
        MemoryPrimitives.WriteU64(b, 164, now);     // created
        MemoryPrimitives.WriteU64(b, 172, now);     // modified
        MemoryPrimitives.WriteU64(b, 180, now);     // accessed
        WriteEntryRaw((uint)id, b);
        return id;
    }

    /// <summary>Rename entry in place (parent unchanged). Returns true on success.</summary>
    public bool Rename(uint id, string newName)
    {
        if (id == _sb.RootId) return false;
        if (!TryNormalizeName(newName, out var nm)) return false;

        var b = ReadEntryRaw(id);
        if (b[8] == 0) return false;                                  // free slot
        uint parent = MemoryPrimitives.ReadU32(b, 4);
        int existing = FindInDir(parent, nm);
        if (existing >= 0 && existing != id) return false;            // name collision

        Array.Clear(b, NAME_OFF, NAME_LEN);                           // wipe old name
        var nameBytes = Encoding.ASCII.GetBytes(nm);
        Array.Copy(nameBytes, 0, b, NAME_OFF, Math.Min(nameBytes.Length, NAME_LEN - 1));
        b[10] = (byte)nm.Length;                                      // name_len
        MemoryPrimitives.WriteU64(b, 172, Now());                     // modified
        WriteEntryRaw(id, b);
        return true;
    }

    /// <summary>Delete an entry; recurses into directories. Frees file data blocks. Returns true on success.</summary>
    public bool Delete(uint id)
    {
        if (id == _sb.RootId) return false;
        var e = ReadEntry(id);
        if (e.IsFree) return false;

        if (e.IsDirectory)
            foreach (var child in ChildrenOf(id)) Delete(child);
        else if (e.IsFile)
            for (int i = 0; i < 8; i++)
                for (uint blk = 0; blk < e.ExtentLen[i]; blk++)
                    FreeBlock(e.ExtentStart[i] + blk);

        // zero the slot, keep id, mark FREE (matches cxfs_delete_entry)
        var b = new byte[ES];
        MemoryPrimitives.WriteU32(b, 0, id);
        // b[8] type = 0 (FREE) already
        WriteEntryRaw(id, b);
        return true;
    }

    private void FreeBlock(uint block)
    {
        if (block < _sb.DataStart) return;                 // never touch system region
        long byteOff = _volumeBase + (long)_sb.BitmapStart * BS + block / 8;
        _disk.Position = byteOff;
        int cur = _disk.ReadByte();
        if (cur < 0) return;
        cur &= ~(1 << (int)(block % 8));                   // LSB-first, clear bit
        _disk.Position = byteOff;
        _disk.WriteByte((byte)cur);
        _disk.Flush();
    }

    public void Dispose() { /* caller owns the stream */ }
}