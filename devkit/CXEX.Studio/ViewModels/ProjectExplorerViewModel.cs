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
                => new ImageEditorViewModel(),
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
            case ImageEditorViewModel img: img.LoadImage(node.FullPath); break;
        }

        docDock.VisibleDockables.Add(tab);
        docDock.ActiveDockable = tab;
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