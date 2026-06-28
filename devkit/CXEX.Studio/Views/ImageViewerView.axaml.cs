using System;
using System.ComponentModel;
using System.IO;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media.Imaging;
using CXEX.Studio.ViewModels;

namespace CXEX.Studio.Views;

public partial class ImageViewerView : UserControl
{
    private ImageViewerViewModel? _vm;
    private string? _loadedPath;

    public ImageViewerView() => InitializeComponent();

    protected override void OnDataContextChanged(EventArgs e)
    {
        base.OnDataContextChanged(e);
        if (_vm is not null) _vm.PropertyChanged -= OnVmPropertyChanged;
        _vm = DataContext as ImageViewerViewModel;
        if (_vm is not null) _vm.PropertyChanged += OnVmPropertyChanged;
        TryLoad();
    }

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        TryLoad();
    }

    private void OnVmPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(ImageViewerViewModel.ImagePath)) TryLoad();
    }

    private void TryLoad()
    {
        var path = _vm?.ImagePath;
        if (string.IsNullOrEmpty(path) || path == _loadedPath || !File.Exists(path)) return;
        try
        {
            PreviewImage.Source = new Bitmap(path);
        }
        catch (Exception)
        {
            // not a decodable raster image (e.g. .ico, a disk image, or a non-image file)
            PreviewImage.Source = null;
        }
        _loadedPath = path;   // record either way so we never retry the same bad path
    }
}