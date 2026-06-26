using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls.ApplicationLifetimes;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CXEX.Studio.Models;
using CXEX.Studio.Services;
using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm.Controls;

namespace CXEX.Studio.ViewModels;

public partial class ProjectExplorerViewModel : Tool
{
    [ObservableProperty] private ObservableCollection<FileTreeNode> _rootNodes = new();
    [ObservableProperty] private string _currentProjectPath = string.Empty;

    private readonly FileTypeInspectorViewModel _hexInspector;
    private readonly ImageViewModel _imageExplorer;
    private readonly ProjectService _projects = new();

    public ProjectExplorerViewModel(FileTypeInspectorViewModel hexInspector, ImageViewModel imageExplorer)
    {
        Id = "ProjectExplorer";
        Title = "Explorer";
        _hexInspector = hexInspector;
        _imageExplorer = imageExplorer;

        // Convenience: auto-open a sibling CXK tree if present (dev workflow).
        string guess = Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, "..", "..", "CXK"));
        if (Directory.Exists(guess)) LoadDirectory(guess);
    }

    /// <summary>Load a project root. Tree expands lazily via ProjectService.</summary>
    public void LoadDirectory(string rootPath)
    {
        RootNodes.Clear();
        CurrentProjectPath = rootPath;
        var root = _projects.BuildTree(rootPath);
        if (root != null) RootNodes.Add(root);
    }

    [RelayCommand]
    private void OpenFile(FileTreeNode? node)
    {
        if (node == null || node.IsDirectory || string.IsNullOrEmpty(node.FullPath)) return;

        var lifetime = Avalonia.Application.Current?.ApplicationLifetime as IClassicDesktopStyleApplicationLifetime;
        if (lifetime?.MainWindow?.DataContext is not MainWindowViewModel mainVm || mainVm.Layout is null) return;

        var docDock = FindDocumentDock(mainVm.Layout);
        if (docDock?.VisibleDockables is null) return;

        // Dedupe: if this file is already open, just activate its tab.
        var existing = docDock.VisibleDockables.FirstOrDefault(d => d.Id == node.FullPath);
        if (existing != null) { docDock.ActiveDockable = existing; return; }

        string ext = Path.GetExtension(node.FullPath).ToLowerInvariant();
        bool isDisk = ext is ".img" or ".iso" or ".bin" or ".vhd" or ".disk" or ".dd";

        Document tab = ext switch
        {
            ".c" or ".h" or ".hpp" or ".cpp" or ".ld" or ".lds" or ".asm" or ".nasm" or ".s" or ".inc"
                or ".xfxn" or ".xfxr" or ".xfxh" or ".txt" or ".cmake" or ".json" or ".md"
                => new TextEditorViewModel(),
            ".png" or ".ico" or ".bmp" or ".jpg" or ".jpeg" or ".gif"
                => new ImageViewerViewModel(),
            // disk images + anything else -> the disk-paged hex inspector
            _ => new FileTypeInspectorViewModel()
        };
        tab.Id = node.FullPath;
        tab.Title = (isDisk ? "Disk: " : "") + node.Name;

        // Set the source path BEFORE the view is built so it loads on first show.
        switch (tab)
        {
            case TextEditorViewModel te: te.LoadFile(node.FullPath); break;
            case FileTypeInspectorViewModel hex: hex.LoadFile(node.FullPath); break;
            case ImageViewerViewModel img: img.LoadImage(node.FullPath); break;
        }

        docDock.VisibleDockables.Add(tab);
        docDock.ActiveDockable = tab;
    }

    // ===== context-menu operations =====
    private string? _clipPath;
    private bool _clipCut;
    public bool CanPaste => _clipPath != null;
    public FileTreeNode? RootNode => RootNodes.FirstOrDefault();

    /// <summary>Directory to operate inside for a right-clicked node (folder -> itself, file -> its parent, empty -> root).</summary>
    public FileTreeNode? TargetDir(FileTreeNode? node)
        => node == null ? RootNode : node.IsDirectory ? node : (node.Parent ?? RootNode);

    public void NewFile(FileTreeNode? node, string name)
    {
        var dir = TargetDir(node); if (dir == null || string.IsNullOrWhiteSpace(name)) return;
        try { var p = Path.Combine(dir.FullPath, name); if (!File.Exists(p)) File.Create(p).Dispose(); _projects.RefreshNode(dir); } catch { }
    }

    public void NewFolder(FileTreeNode? node, string name)
    {
        var dir = TargetDir(node); if (dir == null || string.IsNullOrWhiteSpace(name)) return;
        try { Directory.CreateDirectory(Path.Combine(dir.FullPath, name)); _projects.RefreshNode(dir); } catch { }
    }

    public void Rename(FileTreeNode? node, string newName)
    {
        if (node == null || string.IsNullOrWhiteSpace(newName) || string.IsNullOrEmpty(node.FullPath)) return;
        try
        {
            var dest = Path.Combine(Path.GetDirectoryName(node.FullPath)!, newName);
            if (node.IsDirectory) Directory.Move(node.FullPath, dest); else File.Move(node.FullPath, dest);
            var refresh = node.Parent ?? RootNode; if (refresh != null) _projects.RefreshNode(refresh);
        }
        catch { }
    }

    public void Delete(FileTreeNode? node)
    {
        if (node == null || string.IsNullOrEmpty(node.FullPath)) return;
        try
        {
            if (node.IsDirectory) Directory.Delete(node.FullPath, true); else File.Delete(node.FullPath);
            var refresh = node.Parent ?? RootNode; if (refresh != null) _projects.RefreshNode(refresh);
        }
        catch { }
    }

    public void Cut(FileTreeNode? node) { if (!string.IsNullOrEmpty(node?.FullPath)) { _clipPath = node!.FullPath; _clipCut = true; OnPropertyChanged(nameof(CanPaste)); } }
    public void Copy(FileTreeNode? node) { if (!string.IsNullOrEmpty(node?.FullPath)) { _clipPath = node!.FullPath; _clipCut = false; OnPropertyChanged(nameof(CanPaste)); } }

    public void Paste(FileTreeNode? node)
    {
        var dir = TargetDir(node); if (dir == null || _clipPath == null) return;
        try
        {
            string name = Path.GetFileName(_clipPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            string dest = Path.Combine(dir.FullPath, name);
            if (Path.GetFullPath(_clipPath) == Path.GetFullPath(dest)) return;
            bool isDir = Directory.Exists(_clipPath);
            if (_clipCut)
            {
                if (isDir) Directory.Move(_clipPath, dest); else File.Move(_clipPath, dest);
                _clipPath = null; OnPropertyChanged(nameof(CanPaste));
            }
            else { if (isDir) CopyDir(_clipPath, dest); else File.Copy(_clipPath, dest, false); }
            _projects.RefreshNode(dir);
        }
        catch { }
    }

    private static void CopyDir(string src, string dest)
    {
        Directory.CreateDirectory(dest);
        foreach (var f in Directory.GetFiles(src)) File.Copy(f, Path.Combine(dest, Path.GetFileName(f)), false);
        foreach (var d in Directory.GetDirectories(src)) CopyDir(d, Path.Combine(dest, Path.GetFileName(d)));
    }

    /// <summary>Force-open a file in a specific viewer (Open With).</summary>
    public void OpenAs(FileTreeNode? node, string kind)
    {
        if (node == null || node.IsDirectory || string.IsNullOrEmpty(node.FullPath)) return;
        var dock = GetDocDock(); if (dock?.VisibleDockables is null) return;
        string id = node.FullPath + "#" + kind;
        var existing = dock.VisibleDockables.FirstOrDefault(d => d.Id == id);
        if (existing != null) { dock.ActiveDockable = existing; return; }

        Document tab = kind switch
        {
            "text" => new TextEditorViewModel(),
            "image" => new ImageViewerViewModel(),
            _ => new FileTypeInspectorViewModel(),
        };
        tab.Id = id; tab.Title = $"{node.Name} ({kind})";
        switch (tab)
        {
            case TextEditorViewModel te: te.LoadFile(node.FullPath); break;
            case FileTypeInspectorViewModel hex: hex.LoadFile(node.FullPath); break;
            case ImageViewerViewModel img: img.LoadImage(node.FullPath); break;
        }
        dock.VisibleDockables.Add(tab); dock.ActiveDockable = tab;
    }

    private DocumentDock? GetDocDock()
    {
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime as IClassicDesktopStyleApplicationLifetime;
        if (lifetime?.MainWindow?.DataContext is not MainWindowViewModel mainVm || mainVm.Layout is null) return null;
        return FindDocumentDock(mainVm.Layout);
    }

    private static DocumentDock? FindDocumentDock(IDockable? node)
    {
        if (node is DocumentDock d) return d;
        if (node is IDock dock && dock.VisibleDockables != null)
            foreach (var child in dock.VisibleDockables)
            {
                var found = FindDocumentDock(child);
                if (found != null) return found;
            }
        return null;
    }
}