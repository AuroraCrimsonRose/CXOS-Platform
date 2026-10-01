using CommunityToolkit.Mvvm.ComponentModel;
using Dock.Model.Mvvm.Controls;

namespace CXEX.Studio.ViewModels;

public partial class ImageViewerViewModel : Document
{
    [ObservableProperty] private string? _imagePath;

    public void LoadImage(string path) => ImagePath = path;
}