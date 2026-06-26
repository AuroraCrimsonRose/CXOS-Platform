using Avalonia.Controls;
using Avalonia.Input;
using CXEX.Studio.Models;
using CXEX.Studio.ViewModels;

namespace CXEX.Studio.Views;

public partial class FileTypeInspectorView : UserControl
{
    public FileTypeInspectorView() => InitializeComponent();

    private void OnCxfsDoubleTapped(object? sender, TappedEventArgs e)
    {
        if (DataContext is FileTypeInspectorViewModel vm && CxfsTreeView.SelectedItem is CxfsNode n)
            vm.SelectCxfsNodeCommand.Execute(n);
    }
}