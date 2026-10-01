using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using CXEX.Studio.Dialogs;
using CXEX.Studio.Models;
using CXEX.Studio.ViewModels;

namespace CXEX.Studio.Views;

public partial class ProjectExplorerView : UserControl
{
    public ProjectExplorerView() => InitializeComponent();

    private ProjectExplorerViewModel? Vm => DataContext as ProjectExplorerViewModel;
    private static FileTreeNode? NodeOf(object? sender) => (sender as Control)?.DataContext as FileTreeNode;
    private Window? Owner => TopLevel.GetTopLevel(this) as Window;

    // double-click: open a file, toggle a folder
    private void OnItemDoubleTapped(object? sender, TappedEventArgs e)
    {
        if (NodeOf(sender) is not { } node || Vm is null) return;
        if (node.IsDirectory) node.IsExpanded = !node.IsExpanded;
        else Vm.OpenFileCommand.Execute(node);
    }

    private void OnOpen(object? sender, RoutedEventArgs e)
    { if (NodeOf(sender) is { } n) Vm?.OpenFileCommand.Execute(n); }

    private void OnOpenAsText(object? s, RoutedEventArgs e) => Vm?.OpenAs(NodeOf(s), "text");
    private void OnOpenAsHex(object? s, RoutedEventArgs e) => Vm?.OpenAs(NodeOf(s), "hex");
    private void OnOpenAsImage(object? s, RoutedEventArgs e) => Vm?.OpenAs(NodeOf(s), "image");

    private async void OnNewFile(object? sender, RoutedEventArgs e)
    {
        if (Vm is null || Owner is null) return;
        var name = await PromptWindow.Text(Owner, "New File", "File name:", "untitled.txt");
        if (!string.IsNullOrWhiteSpace(name)) Vm.NewFile(NodeOf(sender), name);
    }

    private async void OnNewFolder(object? sender, RoutedEventArgs e)
    {
        if (Vm is null || Owner is null) return;
        var name = await PromptWindow.Text(Owner, "New Folder", "Folder name:", "New Folder");
        if (!string.IsNullOrWhiteSpace(name)) Vm.NewFolder(NodeOf(sender), name);
    }

    private async void OnRename(object? sender, RoutedEventArgs e)
    {
        if (NodeOf(sender) is not { } node || Vm is null || Owner is null) return;
        var name = await PromptWindow.Text(Owner, "Rename", "New name:", node.Name);
        if (!string.IsNullOrWhiteSpace(name) && name != node.Name) Vm.Rename(node, name);
    }

    private async void OnDelete(object? sender, RoutedEventArgs e)
    {
        if (NodeOf(sender) is not { } node || Vm is null || Owner is null) return;
        if (await PromptWindow.Confirm(Owner, "Delete", $"Delete \"{node.Name}\"? This cannot be undone."))
            Vm.Delete(node);
    }

    private void OnCut(object? s, RoutedEventArgs e) => Vm?.Cut(NodeOf(s));
    private void OnCopy(object? s, RoutedEventArgs e) => Vm?.Copy(NodeOf(s));
    private void OnPaste(object? s, RoutedEventArgs e) => Vm?.Paste(NodeOf(s));
}