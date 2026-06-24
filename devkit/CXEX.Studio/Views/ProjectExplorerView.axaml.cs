using Avalonia.Controls;
using Avalonia.Input;
using CXEX.Studio.Models;
using CXEX.Studio.ViewModels;

namespace CXEX.Studio.Views;

public partial class ProjectExplorerView : UserControl
{
    public ProjectExplorerView()
    {
        InitializeComponent();
    }

    // Safely routes the Avalonia UI event into our MVVM Command architecture
    private void OnItemDoubleTapped(object? sender, TappedEventArgs e)
    {
        // 1. Grab the specific node we clicked on
        if (sender is Control control && control.DataContext is FileTreeNode node)
        {
            // 2. Grab the ViewModel for the whole panel
            if (DataContext is ProjectExplorerViewModel vm)
            {
                // 3. Execute the command
                vm.OpenFileCommand.Execute(node);
                e.Handled = true;
            }
        }
    }
}