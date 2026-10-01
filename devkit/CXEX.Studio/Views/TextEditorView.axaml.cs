using System;
using System.ComponentModel;
using System.IO;
using Avalonia;
using Avalonia.Controls;
using AvaloniaEdit.Highlighting;
using CXEX.Studio.ViewModels;

namespace CXEX.Studio.Views;

public partial class TextEditorView : UserControl
{
    private TextEditorViewModel? _vm;
    private string? _loadedPath;

    public TextEditorView() => InitializeComponent();

    protected override void OnDataContextChanged(EventArgs e)
    {
        base.OnDataContextChanged(e);
        if (_vm is not null) _vm.PropertyChanged -= OnVmPropertyChanged;
        _vm = DataContext as TextEditorViewModel;
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
        if (e.PropertyName == nameof(TextEditorViewModel.FilePath)) TryLoad();
    }

    private void TryLoad()
    {
        var path = _vm?.FilePath;
        if (string.IsNullOrEmpty(path) || path == _loadedPath || !File.Exists(path)) return;

        Editor.Text = File.ReadAllText(path);   // direct + reliable
        _loadedPath = path;

        string ext = Path.GetExtension(path);
        Editor.SyntaxHighlighting = CXEX.Studio.Services.HighlightingProvider.ForExtension(ext);
    }
}