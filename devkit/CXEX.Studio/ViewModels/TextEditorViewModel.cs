using CXEX.Studio.Views;
using Dock.Model.Mvvm.Controls;
using System.Linq;
using Avalonia.Controls;
using Avalonia.Markup.Xaml.Templates;

namespace CXEX.Studio.ViewModels;

public class TextEditorViewModel : Document
{
    public void LoadFile(string path) => ((TextEditorView)Avalonia.Application.Current.DataTemplates.First(x => x is DataTemplate && ((DataTemplate)x).DataType == typeof(TextEditorViewModel)).Build(this)).LoadFile(path);
}
