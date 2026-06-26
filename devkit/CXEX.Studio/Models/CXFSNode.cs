using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CXEX.FileSystem.Volume;

namespace CXEX.Studio.Models;

/// <summary>A node in the CXFS browse tree, wrapping a CXFSEntry from CXEX.FileSystem.</summary>
public partial class CxfsNode : ObservableObject
{
    public CXFSEntry Entry { get; }
    private readonly uint _blockSize;

    public CxfsNode(CXFSEntry entry, uint blockSize) { Entry = entry; _blockSize = blockSize == 0 ? 4096u : blockSize; }

    public string Name => string.IsNullOrEmpty(Entry.Name) ? "/" : Entry.Name;
    public bool IsDirectory => Entry.IsDirectory;
    public string Icon => Entry.IsDirectory ? "Folder" : "FileOutline";
    public string SizeDisplay => Entry.IsDirectory ? "" :
        Entry.Size < 1024 ? $"{Entry.Size} B" : $"{Entry.Size / 1024.0:F1} KB";
    public long FirstExtentOffset => (long)Entry.ExtentStart[0] * _blockSize;

    public ObservableCollection<CxfsNode> Children { get; } = new();
}