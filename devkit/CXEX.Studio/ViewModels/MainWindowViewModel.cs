using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CommunityToolkit.Mvvm.Messaging;
using CXEX.Studio.Docking;
using CXEX.Studio.Messages;
using Dock.Model.Controls;

namespace CXEX.Studio.ViewModels;

public partial class MainWindowViewModel : ObservableObject
{
    private readonly StudioDockFactory _factory;

    [ObservableProperty] private IRootDock? _layout;
    [ObservableProperty] private bool _isBusy;
    [ObservableProperty] private string _busyText = "Working...";
    [ObservableProperty] private bool _isBottomPanelVisible = true;

    /// <summary>The Build output console hosted in the bottom panel.</summary>
    public BuildViewModel Build { get; } = new();

    public MainWindowViewModel()
    {
        _factory = new StudioDockFactory();
        Layout = _factory.CreateLayout();
        if (Layout is { }) _factory.InitLayout(Layout);

        WeakReferenceMessenger.Default.Register<GlobalBusyMessage>(this, (r, m) =>
        {
            IsBusy = m.Value.IsBusy;
            BusyText = m.Value.StatusText;
        });
    }

    [RelayCommand] private void ShowExplorer() => _factory.FocusExplorer();
    [RelayCommand] private void OpenDashboard() => _factory.OpenDashboard();
    [RelayCommand] private void OpenEmulator() => _factory.OpenEmulator();
    [RelayCommand] private void OpenHexInspector() => _factory.OpenHexInspector();
    [RelayCommand] private void ToggleBottomPanel() => IsBottomPanelVisible = !IsBottomPanelVisible;

    public void CloseLayout()
    {
        if (Layout is { }) Layout = null;
    }
}