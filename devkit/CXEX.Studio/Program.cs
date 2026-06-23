using Avalonia;
using System;

namespace CXEX.Studio;

class Program
{
    // Initialization code. Don't use any Visual Studio window type here;
    // Avalonia configuration required before any UI framework is invoked.
    [STAThread]
    public static void Main(string[] args) => BuildAvaloniaApp()
        .StartWithClassicDesktopLifetime(args);

    // Avalonia configuration, don't remove; also used by visual designer.
    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}