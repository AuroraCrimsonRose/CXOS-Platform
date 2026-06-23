using Avalonia.Controls;
using CommunityToolkit.Mvvm.ComponentModel;
using CXEX.Studio.Docking;
using Dock.Model.Controls;
using Dock.Model.Core;

namespace CXEX.Studio.ViewModels;

public partial class MainWindowViewModel : ObservableObject
{
    private readonly IFactory _factory;

    [ObservableProperty]
    private IRootDock? _layout;

    public MainWindowViewModel()
    {
        _factory = new StudioDockFactory();

        // Generate the layout from our factory definition
        Layout = _factory.CreateLayout();

        if (Layout is { })
        {
            _factory.InitLayout(Layout);
        }
    }

    // Optional: Clean up dock factory on close

    public void CloseLayout()
    {
        if (Layout is { })
        {
            // Simply detach the layout on shutdown 
            Layout = null;
        }
    }
}