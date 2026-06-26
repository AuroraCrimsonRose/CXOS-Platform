using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;

namespace CXEX.Studio.Dialogs;

/// <summary>Tiny modal used for New/Rename (text) and Delete (confirm).</summary>
public partial class PromptWindow : Window
{
    public PromptWindow()
    {
        InitializeComponent();
        KeyDown += (_, e) =>
        {
            if (e.Key == Key.Enter) OnOk(this, new RoutedEventArgs());
            else if (e.Key == Key.Escape) OnCancel(this, new RoutedEventArgs());
        };
    }

    private void OnOk(object? sender, RoutedEventArgs e) => Close(Input.IsVisible ? Input.Text ?? "" : "yes");
    private void OnCancel(object? sender, RoutedEventArgs e) => Close(null);

    /// <summary>Prompt for text; returns the entered string, or null if cancelled.</summary>
    public static Task<string?> Text(Window owner, string title, string message, string initial = "")
    {
        var w = new PromptWindow { Title = title };
        w.MessageText.Text = message;
        w.Input.IsVisible = true;
        w.Input.Text = initial;
        w.Input.SelectAll();
        return w.ShowDialog<string?>(owner);
    }

    /// <summary>Confirm dialog; returns true on OK.</summary>
    public static async Task<bool> Confirm(Window owner, string title, string message)
    {
        var w = new PromptWindow { Title = title };
        w.MessageText.Text = message;
        w.Input.IsVisible = false;
        return await w.ShowDialog<string?>(owner) == "yes";
    }
}
