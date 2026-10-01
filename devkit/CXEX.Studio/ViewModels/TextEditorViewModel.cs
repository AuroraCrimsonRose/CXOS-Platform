using CommunityToolkit.Mvvm.ComponentModel;
using Dock.Model.Mvvm.Controls;

namespace CXEX.Studio.ViewModels;

/// <summary>
/// Holds the file path only. The TextEditorView (built by the docking ViewLocator)
/// observes FilePath and loads the file into the editor that is actually shown -
/// no throwaway view, no DataTemplates reflection.
/// </summary>
public partial class TextEditorViewModel : Document
{
    [ObservableProperty] private string? _filePath;

    public void LoadFile(string path) => FilePath = path;
}