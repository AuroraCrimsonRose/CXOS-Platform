using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using CXEX.Studio.ViewModels;
using Dock.Model.Core;

namespace CXEX.Studio.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    private async void OnOpenProjectClicked(object? sender, RoutedEventArgs e)
    {
        // 1. Get the TopLevel window to access the native file system dialogs
        var topLevel = TopLevel.GetTopLevel(this);
        if (topLevel == null) return;

        // 2. Open the OS folder picker
        var result = await topLevel.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Open CXK OS Project Folder",
            AllowMultiple = false
        });

        // 3. If a folder was selected, pass it to the Project Explorer
        if (result.Count > 0 && DataContext is MainWindowViewModel vm)
        {
            string? localPath = result[0].TryGetLocalPath();
            if (!string.IsNullOrEmpty(localPath))
            {
                var explorer = FindProjectExplorer(vm.Layout);
                explorer?.LoadDirectory(localPath);
            }
        }
    }

    // Helper method to dig through the Dock.Avalonia layout and find the Project Explorer tab
    private ProjectExplorerViewModel? FindProjectExplorer(IDockable? node)
    {
        if (node is ProjectExplorerViewModel explorer) return explorer;

        if (node is IDock dock && dock.VisibleDockables != null)
        {
            foreach (var child in dock.VisibleDockables)
            {
                var found = FindProjectExplorer(child);
                if (found != null) return found;
            }
        }

        return null;
    }
}