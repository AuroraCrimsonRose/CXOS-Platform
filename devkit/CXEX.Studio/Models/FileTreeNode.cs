using System;
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;

namespace CXEX.Studio.Models;

public partial class FileTreeNode : ObservableObject
{
    [ObservableProperty] private string _name = string.Empty;
    [ObservableProperty] private string _fullPath = string.Empty;
    [ObservableProperty] private bool _isDirectory;
    [ObservableProperty] private bool _isExpanded;
    [ObservableProperty] private string _icon = "FileOutline";
    [ObservableProperty] private string _iconColor = "#007ACC";
    [ObservableProperty] private string _textColor = "#CCCCCC";

    /// <summary>True once real children have been loaded (lazy loading).</summary>
    [ObservableProperty] private bool _childrenLoaded;

    /// <summary>Set by ProjectService on directory nodes; invoked on first expand.</summary>
    public Action<FileTreeNode>? LazyLoad;

    public ObservableCollection<FileTreeNode> Children { get; } = new();

    /// <summary>Parent directory node (null for the root). Set by ProjectService.</summary>
    public FileTreeNode? Parent { get; set; }

    public FileTreeNode(string name, string fullPath, bool isDirectory)
    {
        Name = name;
        FullPath = fullPath;
        IsDirectory = isDirectory;
        Icon = DetermineIcon(name, isDirectory);
        IconColor = isDirectory ? "#DCCA87" : "#007ACC";
        TextColor = isDirectory ? "#E8E8E8" : "#CCCCCC";
    }

    // Lazy-load children the first time a directory node is expanded.
    partial void OnIsExpandedChanged(bool value)
    {
        if (value && IsDirectory && !ChildrenLoaded)
            LazyLoad?.Invoke(this);
    }

    /// <summary>A throwaway child so the TreeView shows an expander before real load.</summary>
    public static FileTreeNode Placeholder() => new("…", string.Empty, false);

    private static string DetermineIcon(string fileName, bool isDirectory)
    {
        if (isDirectory) return "FolderOutline";
        string ext = System.IO.Path.GetExtension(fileName).ToLower();
        return ext switch
        {
            ".c" or ".h" => "LanguageC",
            ".asm" or ".nasm" => "Memory",
            ".x" => "AlphaXBox",
            ".txt" or ".cmake" or "cmakelists.txt" => "TextBoxOutline",
            ".sh" or ".bat" or ".ps1" => "ConsoleLine",
            ".img" or ".bin" => "Harddisk",
            ".xkex" or ".xoex" or ".xcex" => "ApplicationCog",
            _ => "FileOutline"
        };
    }
}