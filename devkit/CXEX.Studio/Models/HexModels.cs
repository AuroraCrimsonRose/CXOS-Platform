using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;

namespace CXEX.Studio.Models;

public partial class HexCell : ObservableObject
{
    [ObservableProperty] private string _text = "00";
    [ObservableProperty] private string _ascii = ".";
    [ObservableProperty] private string _background = "Transparent";
    [ObservableProperty] private string _foreground = "#D4D4D4";
    [ObservableProperty] private string _toolTip = string.Empty;
}

public partial class HexRow : ObservableObject
{
    [ObservableProperty] private string _offset = "00000000";
    public ObservableCollection<HexCell> Bytes { get; } = new();
    [ObservableProperty] private string _asciiData = string.Empty;
}

public class HexAnnotation
{
    public long StartOffset { get; set; }
    public long Length { get; set; }
    public string Name { get; set; } = string.Empty;
    public string ColorHex { get; set; } = "#007ACC"; // The background highlight color
}