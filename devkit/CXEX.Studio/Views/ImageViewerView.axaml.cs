using Avalonia.Controls;
using Avalonia.Media.Imaging;
using System;

namespace CXEX.Studio.Views;

public partial class ImageViewerView : UserControl
{
    public ImageViewerView() => InitializeComponent();
    public void LoadImage(string path) => PreviewImage.Source = new Bitmap(path);
}