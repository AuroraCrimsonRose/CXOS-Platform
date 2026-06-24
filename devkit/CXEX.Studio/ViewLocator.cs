using System;
using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Dock.Model.Core;

namespace CXEX.Studio;

public class ViewLocator : IDataTemplate
{
    public Control? Build(object? data)
    {
        if (data is null) return null;

        // Replaces "ViewModel" with "View" (e.g., EmulatorViewModel -> EmulatorView)
        var name = data.GetType().FullName!.Replace("ViewModel", "View", StringComparison.Ordinal);
        var type = Type.GetType(name);

        if (type != null)
        {
            return (Control)Activator.CreateInstance(type)!;
        }

        return new TextBlock { Text = "Not Found: " + name };
    }

    public bool Match(object? data)
    {
        // This ensures the locator ONLY triggers for Dock panels (Documents/Tools)
        return data is IDockable;
    }
}