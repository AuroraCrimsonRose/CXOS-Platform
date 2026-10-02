// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.IO;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using CXEX.Studio.Dialogs;
using CXEX.Studio.Models.Project;
using CXEX.Studio.Services;
using CXEX.Studio.ViewModels;
using Dock.Model.Core;

namespace CXEX.Studio.Views;

public partial class MainWindow : Window
{
    public MainWindow() => InitializeComponent();

    private async void OnNewProjectClicked(object? sender, RoutedEventArgs e)
    {
        var topLevel = TopLevel.GetTopLevel(this);
        if (topLevel is null) return;

        var name = await PromptWindow.Text(this, "New Project", "Project name:", "MyProject");
        if (string.IsNullOrWhiteSpace(name)) return;

        var folders = await topLevel.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Choose where to create the project",
            AllowMultiple = false
        });
        if (folders.Count == 0) return;

        var baseDir = folders[0].TryGetLocalPath();
        if (string.IsNullOrEmpty(baseDir)) return;

        var projDir = Path.Combine(baseDir, name);
        var project = CxProjectService.Create(name, projDir);
        ApplyProject(project);
    }

    private async void OnOpenProjectClicked(object? sender, RoutedEventArgs e)
    {
        var topLevel = TopLevel.GetTopLevel(this);
        if (topLevel is null) return;

        var files = await topLevel.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Open CX Project",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("CX Project") { Patterns = new[] { "*" + CxProjectService.Extension } }
            }
        });
        if (files.Count == 0) return;

        var path = files[0].TryGetLocalPath();
        if (string.IsNullOrEmpty(path)) return;

        var project = CxProjectService.Load(path);
        ApplyProject(project);
    }

    private void ApplyProject(CxProject project)
    {
        if (DataContext is not MainWindowViewModel vm) return;
        vm.CurrentProject = project;
        if (project.ProjectDir is { } dir)
            FindProjectExplorer(vm.Layout)?.LoadDirectory(dir);
    }

    private ProjectExplorerViewModel? FindProjectExplorer(IDockable? node)
    {
        if (node is ProjectExplorerViewModel explorer) return explorer;
        if (node is IDock dock && dock.VisibleDockables != null)
            foreach (var child in dock.VisibleDockables)
            {
                var found = FindProjectExplorer(child);
                if (found != null) return found;
            }
        return null;
    }
}