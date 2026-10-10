// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using CXEX.Studio.Dialogs;
using CXEX.Studio.Models;
using CXEX.Studio.ViewModels;

namespace CXEX.Studio.Views;

public partial class FileTypeInspectorView : UserControl
{
    public FileTypeInspectorView() => InitializeComponent();

    private FileTypeInspectorViewModel? Vm => DataContext as FileTypeInspectorViewModel;
    private static CxfsNode? NodeOf(object? sender) => (sender as Control)?.DataContext as CxfsNode;
    private Window? Owner => TopLevel.GetTopLevel(this) as Window;

    // double-click a CXFS file -> jump hex to its data
    private void OnCxfsDoubleTapped(object? sender, TappedEventArgs e)
    {
        if (Vm is { } vm && CxfsTreeView.SelectedItem is CxfsNode n) vm.SelectCxfsNodeCommand.Execute(n);
    }

    private async void OnCxfsSave(object? sender, RoutedEventArgs e)
    {
        if (NodeOf(sender) is not { IsDirectory: false } node || Vm is null || Owner is null) return;
        var bytes = Vm.CxfsReadFile(node);
        if (bytes is null) return;
        var file = await Owner.StorageProvider.SaveFilePickerAsync(
            new FilePickerSaveOptions { Title = "Save file to host", SuggestedFileName = node.Name });
        if (file is null) return;
        await using var stream = await file.OpenWriteAsync();
        await stream.WriteAsync(bytes);
    }

    private async void OnCxfsNewFolder(object? sender, RoutedEventArgs e)
    {
        if (Vm is null || Owner is null) return;
        var name = await PromptWindow.Text(Owner, "New Folder", "Folder name:", "New Folder");
        if (!string.IsNullOrWhiteSpace(name)) Vm.CxfsNewFolder(NodeOf(sender), name);
    }

    private async void OnCxfsRename(object? sender, RoutedEventArgs e)
    {
        if (NodeOf(sender) is not { } node || Vm is null || Owner is null) return;
        var name = await PromptWindow.Text(Owner, "Rename", "New name:", node.Name);
        if (!string.IsNullOrWhiteSpace(name) && name != node.Name) Vm.CxfsRename(node, name);
    }

    private async void OnCxfsDelete(object? sender, RoutedEventArgs e)
    {
        if (NodeOf(sender) is not { } node || Vm is null || Owner is null) return;
        string msg = node.IsDirectory ? $"Delete \"{node.Name}\" and everything inside it?" : $"Delete \"{node.Name}\"?";
        if (await PromptWindow.Confirm(Owner, "Delete", msg)) Vm.CxfsDelete(node);
    }
}