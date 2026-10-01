using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using CXEX.Studio.ViewModels;
using CXEX.Studio.Views;
using CXEX.Studio.Docking;

namespace CXEX.Studio;

public partial class App : Application
{
    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            // Set up our newly fixed factory layouts
            var factory = new StudioDockFactory();
            var layout = factory.CreateLayout();

            if (layout is { })
            {
                factory.InitLayout(layout);
            }

            // Bind the view model and supply the layout data context
            var viewModel = new MainWindowViewModel
            {
                Layout = layout
            };

            desktop.MainWindow = new MainWindow
            {
                DataContext = viewModel
            };
        }

        base.OnFrameworkInitializationCompleted();
    }
}