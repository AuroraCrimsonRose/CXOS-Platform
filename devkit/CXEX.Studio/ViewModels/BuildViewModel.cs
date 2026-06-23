using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace CXEX.Studio.ViewModels;

public partial class BuildViewModel : ObservableObject
{
    [ObservableProperty]
    private string projectPath = string.Empty;

    [ObservableProperty]
    private int buildProgress;

    [ObservableProperty]
    private string buildStatus = "Ready";

    [RelayCommand]
    private void Build()
    {
        BuildStatus = "Building...";
        BuildProgress = 100;
        BuildStatus = "Build Complete";
    }
}