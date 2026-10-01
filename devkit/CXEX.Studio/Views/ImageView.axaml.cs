using Avalonia.Controls;
using Avalonia.Input;
using CXEX.Studio.ViewModels;

namespace CXEX.Studio.Views;

public partial class ImageView : UserControl
{
    public ImageView()
    {
        InitializeComponent();
    }

    private void OnDiskItemDoubleTapped(object? sender, TappedEventArgs e)
    {
        if (sender is Control control && control.DataContext is ImageNode node)
        {
            if (DataContext is ImageViewModel vm)
            {
                vm.OpenNodeInHexCommand.Execute(node);
                e.Handled = true;
            }
        }
    }
}