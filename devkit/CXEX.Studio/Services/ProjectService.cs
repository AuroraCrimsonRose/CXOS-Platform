using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using CXEX.Studio.Models;

namespace CXEX.Studio.Services;

/// <summary>
/// Owns project loading and the file tree. The tree loads lazily: only the
/// immediate children of an expanded directory are read from disk (a placeholder
/// gives each directory its expander arrow), so opening a large kernel tree is
/// instant instead of recursively walking thousands of files up front.
/// </summary>
public sealed class ProjectService
{
    public string? CurrentRoot { get; private set; }

    /// <summary>Show the OS folder picker; returns the chosen path (and records it).</summary>
    public async Task<string?> PickFolderAsync(TopLevel topLevel)
    {
        var res = await topLevel.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Open CXK Project Folder",
            AllowMultiple = false
        });
        var path = res.Count > 0 ? res[0].TryGetLocalPath() : null;
        if (!string.IsNullOrEmpty(path)) CurrentRoot = path;
        return path;
    }

    /// <summary>Build the root node (expanded, one level loaded).</summary>
    public FileTreeNode? BuildTree(string root)
    {
        if (!Directory.Exists(root)) return null;
        CurrentRoot = root;
        var node = new FileTreeNode(
            Path.GetFileName(root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)),
            root, true)
        { LazyLoad = LoadChildren };
        LoadChildren(node);
        node.IsExpanded = true;
        return node;
    }

    /// <summary>Load one directory level. Directory children get a placeholder + loader.</summary>
    public void LoadChildren(FileTreeNode node)
    {
        if (!node.IsDirectory || node.ChildrenLoaded) return;
        node.Children.Clear();
        try
        {
            foreach (var dir in Directory.GetDirectories(node.FullPath).OrderBy(d => d))
            {
                var info = new DirectoryInfo(dir);
                if (info.Attributes.HasFlag(FileAttributes.Hidden)) continue;
                var child = new FileTreeNode(Path.GetFileName(dir), dir, true) { LazyLoad = LoadChildren };
                child.Children.Add(FileTreeNode.Placeholder());   // expander shows before real load
                node.Children.Add(child);
            }
            foreach (var file in Directory.GetFiles(node.FullPath).OrderBy(f => f))
            {
                var fi = new FileInfo(file);
                if (fi.Attributes.HasFlag(FileAttributes.Hidden)) continue;
                node.Children.Add(new FileTreeNode(Path.GetFileName(file), file, false));
            }
        }
        catch (UnauthorizedAccessException) { }
        catch (DirectoryNotFoundException) { }
        node.ChildrenLoaded = true;
    }
}