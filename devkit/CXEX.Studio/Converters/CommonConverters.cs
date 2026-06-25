using System;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Data.Converters;

namespace CXEX.Studio.Converters;

/// <summary>bool -> GridLength. True yields the ConverterParameter length (e.g. "220"
/// or "5"); false yields 0 so the row collapses. Used for the collapsible bottom panel.</summary>
public sealed class BoolToGridLengthConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        bool open = value is true;
        if (!open) return new GridLength(0);
        var p = parameter?.ToString();
        return GridLength.Parse(string.IsNullOrEmpty(p) ? "Auto" : p);
    }
    public object ConvertBack(object? value, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>bool -> opacity. True (panel open) dims to 0.5; false (closed) full 1.0.
/// Used so the sidebar toggle is bright when the panel is hidden, dim when shown.</summary>
public sealed class BoolToOpacityConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
        => value is true ? 0.5 : 1.0;
    public object ConvertBack(object? value, Type t, object? p, CultureInfo c) => throw new NotSupportedException();
}