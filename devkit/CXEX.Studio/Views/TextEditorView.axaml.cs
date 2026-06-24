using Avalonia.Controls;
using AvaloniaEdit; // This is the correct namespace for the TextEditor control
using AvaloniaEdit.Highlighting; // This is the correct namespace for HighlightingManager
using System.IO;

namespace CXEX.Studio.Views;

public partial class TextEditorView : UserControl
{
    public TextEditorView()
    {
        InitializeComponent();
    }

    public void LoadFile(string path)
    {
        if (!File.Exists(path)) return;

        // Use the Load method for better performance with larger OS source files
        Editor.Load(path);

        // Map extensions to their syntax definitions
        string ext = Path.GetExtension(path).ToLower();

        Editor.SyntaxHighlighting = ext switch
        {
            ".c" or ".h" or ".cpp" => HighlightingManager.Instance.GetDefinition("C++"),
            ".asm" or ".nasm" => HighlightingManager.Instance.GetDefinition("Assembly"), // Ensure you have an assembly definition loaded
            ".xml" => HighlightingManager.Instance.GetDefinition("XML"),
            _ => null
        };
    }
}