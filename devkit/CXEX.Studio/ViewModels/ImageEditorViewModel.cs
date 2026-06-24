using CXEX.Studio.Views;
using Dock.Model.Mvvm.Controls;
using System.Linq;
using Avalonia.Controls;
using Avalonia.Markup.Xaml.Templates;

namespace CXEX.Studio.ViewModels;

public class ImageEditorViewModel : Document
{
    public void LoadImage(string path) => ((ImageViewerView)Avalonia.Application.Current.DataTemplates.First(x => x is DataTemplate && ((DataTemplate)x).DataType == typeof(ImageEditorViewModel)).Build(this)).LoadImage(path);
}