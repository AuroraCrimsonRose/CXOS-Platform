using Dock.Avalonia.Controls;
using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm;
using Dock.Model.Mvvm.Controls;
using System;
using System.Collections.Generic;

namespace CXEX.Studio.Docking;

public class StudioDockFactory : Factory
{
    public override IRootDock CreateLayout()
    {
        // Define individual tool tabs
        var projectExplorer = new Tool { Id = "ProjectExplorer", Title = "Project Explorer" };
        var hexViewer = new Tool { Id = "HexViewer", Title = "Hex Viewer" };
        var logViewer = new Tool { Id = "LogViewer", Title = "Output / Logs" };

        // Main center document area
        var documentDock = new DocumentDock
        {
            Id = "Documents",
            Title = "Documents",
            IsCollapsable = false,
            Proportion = double.NaN,
            CanCreateDocument = true
        };

        // Construct layout using ToolDock instead of ProportionalDockWindow
        var mainLayout = new ProportionalDock
        {
            Orientation = Orientation.Horizontal,
            VisibleDockables = CreateList<IDockable>
            (
                new ToolDock
                {
                    Id = "LeftPane",
                    Proportion = 0.2,
                    VisibleDockables = CreateList<IDockable>(projectExplorer),
                    ActiveDockable = projectExplorer
                },
                new ProportionalDockSplitter(),
                documentDock,
                new ProportionalDockSplitter(),
                new ToolDock
                {
                    Id = "RightPane",
                    Proportion = 0.25,
                    VisibleDockables = CreateList<IDockable>(hexViewer, logViewer),
                    ActiveDockable = hexViewer
                }
            )
        };

        var rootDock = CreateRootDock();
        rootDock.IsCollapsable = false;
        rootDock.DefaultDockable = mainLayout;
        rootDock.VisibleDockables = CreateList<IDockable>(mainLayout);
        rootDock.ActiveDockable = mainLayout;

        return rootDock;
    }

    public override void InitLayout(IDockable layout)
    {
        // Added '?' to object and IHostWindow to resolve CS8619 warnings
        ContextLocator = new Dictionary<string, Func<object?>>
        {
            // ["ProjectExplorer"] = () => new ProjectExplorerViewModel()
        };

        HostWindowLocator = new Dictionary<string, Func<IHostWindow?>>
        {
            [nameof(IDockWindow)] = () => new HostWindow()
        };

        base.InitLayout(layout);
    }
}